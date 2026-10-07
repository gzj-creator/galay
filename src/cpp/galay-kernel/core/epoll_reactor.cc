/**
 * @file epoll_reactor.cc
 * @brief Linux epoll reactor 实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 Linux epoll 实现IO事件注册、批量提交和事件分发，
 * eventfd 用于跨线程唤醒，inotify 用于文件监控，libaio 用于异步文件IO。
 */

#include "epoll_reactor.h"

#ifdef USE_EPOLL

#include "awaitable.h"
#include "../async/async_aio.h"

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <expected>
#include <string>
#include <vector>

namespace galay::kernel {

namespace {

constexpr int kImmediateReady = 1;

uint32_t io_type_to_epoll_events(IOEventType type) {
    uint32_t events = EPOLLET;
    const uint32_t t = static_cast<uint32_t>(type);
    if (t & (ACCEPT | RECV | READV | RECVFROM | FILEREAD | FILEWATCH)) {
        events |= EPOLLIN;
    }
    if (t & (CONNECT | SEND | WRITEV | SENDTO | SENDFILE | FILEWRITE)) {
        events |= EPOLLOUT;
    }
    return events;
}

uint32_t sequence_interest_to_epoll_events(detail::SequenceInterestMask mask) {
    uint32_t events = EPOLLET;
    if ((mask & detail::sequence_slot_mask(IOController::READ)) != 0) {
        events |= EPOLLIN;
    }
    if ((mask & detail::sequence_slot_mask(IOController::WRITE)) != 0) {
        events |= EPOLLOUT;
    }
    return events;
}

}  // namespace

EpollReactor::EpollReactor(int max_events, std::atomic<uint64_t>& last_error_code)
    : m_max_events(max_events)
    , m_last_error_code(last_error_code) {}

std::expected<void, IOError> EpollReactor::start()
{
    m_accept_stopping = false;
    if (m_epoll_fd != -1 && m_event_fd != -1) {
        return {};
    }

    m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll_fd == -1) {
        detail::store_backend_error(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_event_fd == -1) {
        close(m_epoll_fd);
        m_epoll_fd = -1;
        detail::store_backend_error(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nullptr;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_event_fd, &ev) == -1) {
        close(m_epoll_fd);
        close(m_event_fd);
        m_epoll_fd = -1;
        m_event_fd = -1;
        detail::store_backend_error(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    m_events.resize(m_max_events);
    return {};
}

EpollReactor::~EpollReactor() {
    for (auto& [fd, entry] : m_registration_entries) {
        (void)fd;
        if (entry && entry->controller) {
            entry->controller->release_registration_owner_slot();
        }
    }
    if (m_epoll_fd != -1) {
        close(m_epoll_fd);
    }
    if (m_event_fd != -1) {
        close(m_event_fd);
    }
}

void EpollReactor::notify() {
    uint64_t val = 1;
    if (write(m_event_fd, &val, sizeof(val)) < 0) {
        detail::store_backend_error(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

GHandle EpollReactor::get_handle() const {
    return {m_event_fd};
}

GHandle EpollReactor::get_poll_handle() const {
    return {m_epoll_fd};
}

EpollReactor::RegistrationEntry* EpollReactor::registration_entry_for_controller(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return nullptr;
    }

    const int fd = controller->m_handle.fd;
    auto it = m_registration_entries.find(fd);
    if (it == m_registration_entries.end()) {
        auto entry = std::make_unique<RegistrationEntry>();
        auto* raw = entry.get();
        controller->bind_registration_owner_slot(&raw->controller);
        m_registration_entries.emplace(fd, std::move(entry));
        return raw;
    }

    controller->bind_registration_owner_slot(&it->second->controller);
    return it->second.get();
}

void EpollReactor::retire_registration_entry(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return;
    }

    const int fd = controller->m_handle.fd;
    auto it = m_registration_entries.find(fd);
    if (it == m_registration_entries.end()) {
        return;
    }
    if (it->second->controller != controller) {
        return;
    }

    controller->release_registration_owner_slot();
    m_retired_entries.push_back(std::move(it->second));
    m_registration_entries.erase(it);
}

std::pair<size_t, EpollReactor::RegistrationEntry*>
EpollReactor::find_pending_change(IOController* controller) const {
    const auto entry = m_registration_entries.find(controller->m_handle.fd);
    if (entry == m_registration_entries.end() || entry->second->controller != controller) {
        return {m_pending_changes.size(), nullptr};
    }
    auto* registration = entry->second.get();
    auto it = m_pending_change_index.find(registration);
    if (it != m_pending_change_index.end()) {
        return {it->second, registration};
    }
    return {m_pending_changes.size(), registration};
}

void EpollReactor::erase_pending_change(size_t index) {
    if (index >= m_pending_changes.size()) {
        return;
    }

    // PERF: swap-and-pop 删除，避免移动元素
    // 注销或 controller 析构会清空 entry->controller，但 entry 的地址仍稳定。
    // 用 entry 作 key，才能在这些路径上清除索引，避免后续注册覆盖其他 fd。
    // erase 的计数仅报告是否存在；队列清理无需使用该计数。
    (void)m_pending_change_index.erase(m_pending_changes[index].entry);

    if (index != m_pending_changes.size() - 1) {
        // 将最后一个元素移到被删除位置
        m_pending_changes[index] = std::move(m_pending_changes.back());
        // 更新被移动元素的索引
        m_pending_change_index[m_pending_changes[index].entry] = index;
    }

    m_pending_changes.pop_back();
}

void EpollReactor::discard_pending_change(IOController* controller) {
    const size_t index = find_pending_change(controller).first;
    if (index != m_pending_changes.size()) {
        erase_pending_change(index);
    }
}

uint32_t EpollReactor::build_events(IOController* controller) const {
    if (controller == nullptr) {
        return EPOLLET;
    }

    uint32_t events = io_type_to_epoll_events(controller->m_type);
    events |= controller->m_persistent_events;
    const uint32_t t = static_cast<uint32_t>(controller->m_type);
    if ((t & SEQUENCE) == 0) {
        return events;
    }

    events |= sequence_interest_to_epoll_events(controller->m_sequence_interest_mask);
    return events;
}

int EpollReactor::arm_persistent_write(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }
    if (registration_entry_for_controller(controller) == nullptr) {
        return -1;
    }
    controller->m_persistent_events |= EPOLLOUT;
    return apply_events(controller, build_events(controller));
}

int EpollReactor::arm_persistent_read(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    // 2026-07-14 WS 固定口径：持久 EPOLLIN 令 epoll_ctl 7,022 -> 36，吞吐提升 3.86%。
    // 正确性依赖 add_recv/add_readv 在注册前先做非阻塞乐观读取，即使旧边沿被消费也能直接取走残留数据。
    // 持久注册期间 controller 仍可能移动；稳定入口通过 controller 的反向槽位在移动时原位重绑。
    if (registration_entry_for_controller(controller) == nullptr) {
        return -1;
    }
    controller->m_persistent_events |= EPOLLIN;
    return apply_events(controller, build_events(controller));
}

int EpollReactor::apply_events(IOController* controller, uint32_t events, bool flush_at_threshold) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    const auto [index, registered_entry] = find_pending_change(controller);
    // 未注册状态下的删除是 no-op，不能留下可能跨过 socket 析构的裸 controller 指针。
    if (events == EPOLLET && controller->m_registered_events == 0) {
        if (index != m_pending_changes.size()) {
            erase_pending_change(index);
        }
        return 0;
    }

    if (index != m_pending_changes.size()) {
        if (events == controller->m_registered_events) {
            erase_pending_change(index);
            return 0;
        }
        if (m_pending_changes[index].events == events) {
            return 0;
        }
        m_pending_changes[index].events = events;
    } else {
        if (events == controller->m_registered_events) {
            return 0;
        }
        auto* entry = registered_entry != nullptr
            ? registered_entry
            : registration_entry_for_controller(controller);
        if (entry == nullptr) {
            return -1;
        }
        // PERF: 添加 pending change 时同步更新索引
        const size_t new_index = m_pending_changes.size();
        m_pending_changes.push_back(PendingChange{
            .entry = entry,
            .events = events,
        });
        m_pending_change_index[entry] = new_index;
    }

    if (flush_at_threshold && m_pending_changes.size() >= BATCH_THRESHOLD) {
        return flush_pending_changes();
    }
    return 0;
}

int EpollReactor::update_registration(IOController* controller, uint32_t events) {
    if (events == controller->m_registered_events ||
        (events == EPOLLET && controller->m_registered_events == 0)) { return 0; }

    const int fd = controller->m_handle.fd;
    if (events == EPOLLET) {
        int ret;
        do { ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr); }
        while (ret == -1 && errno == EINTR);
        if (ret == 0 || errno == ENOENT) {
            controller->m_registered_events = 0;
            return 0;
        }
        return -errno;
    }

    epoll_event event{};
    event.events = events;
    event.data.ptr = registration_entry_for_controller(controller);
    if (event.data.ptr == nullptr) { return -EINVAL; }
    const int action = controller->m_registered_events == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    int ret;
    do { ret = epoll_ctl(m_epoll_fd, action, fd, &event); }
    while (ret == -1 && errno == EINTR);
    if (ret == -1 && action == EPOLL_CTL_MOD && errno == ENOENT) {
        do { ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event); }
        while (ret == -1 && errno == EINTR);
    }
    if (ret != 0) { return -errno; }
    controller->m_registered_events = events;
    return 0;
}

int EpollReactor::flush_pending_changes() {
    // FIX: 收集所有需要执行的恢复能力，避免在遍历期间修改状态
    std::vector<ResumeCapability> resumes_to_execute;
    int first_error = 0;

    size_t index = 0;
    while (index < m_pending_changes.size()) {
        PendingChange change = m_pending_changes[index];
        auto* controller = change.entry ? change.entry->controller : nullptr;
        if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
            erase_pending_change(index);
            continue;
        }

        const uint32_t events = change.events;
        if (events == controller->m_registered_events) {
            erase_pending_change(index);
            continue;
        }

        const int ret = update_registration(controller, events);
        if (ret == 0) {
            if (events == EPOLLET) { retire_registration_entry(controller); }
            erase_pending_change(index);
            continue;
        }

        const auto error = static_cast<uint32_t>(-ret);
        if (first_error == 0) {
            first_error = ret;
        }
        detail::store_backend_error(m_last_error_code, kNotReady, error);
        if (events == EPOLLET) {
            // FIX: 安全执行所有收集的恢复
            for (auto& resume : resumes_to_execute) {
                std::move(resume).resume();
            }
            return ret;
        }
        if ((static_cast<uint32_t>(controller->m_type) & ACCEPT) != 0) {
            auto* awaitable = controller->get_awaitable<AcceptAwaitable>();
            if (awaitable && awaitable->select_error(CompletionReason::kBackendError,
                                                  IOError(kAcceptFailed, error))) {
                controller->remove_awaitable(ACCEPT);
                erase_pending_change(index);
                retire_registration_entry(controller);
                auto resume = awaitable->detach();
                if (resume) {
                    // FIX: 保存到容器，稍后执行
                    resumes_to_execute.push_back(std::move(*resume));
                    // erase_pending_change 会把末项换入 index，必须继续处理此位置。
                    continue;
                }
                detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
            }
        }
        // FIX: 安全执行所有收集的恢复
        for (auto& resume : resumes_to_execute) {
            std::move(resume).resume();
        }
        return -static_cast<int>(error);
    }

    // FIX: 安全执行所有恢复（此时不再遍历 m_pending_changes）
    for (auto& resume : resumes_to_execute) {
        std::move(resume).resume();
    }
    return first_error;
}

int EpollReactor::add_accept(IOController* controller) {
    auto* awaitable = controller->get_awaitable<AcceptAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->select_ready(controller->m_handle)) {
        return kImmediateReady;
    }
    // The scheduler flushes after the ready pass, outside await_suspend. A
    // failed older registration may otherwise resume C code that frees this
    // controller while submit_accept still borrows it.
    return apply_events(controller, build_events(controller), false);
}

bool EpollReactor::submit_accept(AcceptAwaitable& awaitable, Waker&& waker) {
    auto* controller = awaitable.m_controller;
    const auto fail = [&](IOError error) {
        if (awaitable.select_error(CompletionReason::kBackendError, error)) {
            const auto resume = awaitable.detach(); // 同步完成，不发布 ready entry。
            if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
        }
        return false;
    };
    const bool valid = controller && controller->m_handle != GHandle::invalid();
    const OperationKey key{valid ? static_cast<uint32_t>(controller->m_handle.fd) : 0,
                           m_next_accept_generation};
    awaitable.m_operation.emplace(key, std::move(waker));
    if (!valid) { return fail(IOError(kClosed, 0)); }
    if (m_accept_stopping) { return fail(IOError(kClosed, 0)); }
    if (m_next_accept_generation == 0) { return fail(IOError(kNotReady, EOVERFLOW)); }
    ++m_next_accept_generation; // uint32 wrap 到 0 后拒绝新提交，不发布重复 key。
    if (controller->m_awaitable[IOController::READ] ||
        controller->m_sequence_owner[IOController::READ]) {
        return fail(IOError(kNotReady, EBUSY));
    }
    if (!awaitable.m_operation->mark_submitted()) { return fail(IOError(kNotReady, EINVAL)); }
    const auto retained = awaitable.m_operation->add_physical_reference();
    if (!retained) { return fail(IOError(kNotReady, EOVERFLOW)); }
    if (!controller->fill_awaitable(ACCEPT, &awaitable)) {
        // FIX: fill_awaitable 失败发生在 add_physical_reference 之后，必须使用 detach_accept
        if (!awaitable.select_error(CompletionReason::kBackendError, IOError(kNotReady, EINVAL))) {
            detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
        }
        const auto resume = detach_accept(awaitable);
        if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
        return false;
    }
    const int result = add_accept(controller);
    if (result == kImmediateReady) {
        // FIX: 同步完成路径必须立即释放 physical reference
        const auto release_result = awaitable.m_operation->release_physical_reference();
        if (!release_result) {
            detail::store_backend_error(m_last_error_code, kNotReady,
                                       static_cast<uint32_t>(OperationError::kNoPhysicalReference));
            return fail(IOError(kNotReady, EAGAIN));
        }
        const auto resume = detach_accept(awaitable);
        if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
        return false;
    }
    if (result != 0) {
        if (result < 0 && !awaitable.m_operation->state().completion_reason() &&
            !awaitable.select_error(CompletionReason::kBackendError,
                IOError(kAcceptFailed, detail::normalize_awaitable_errno(result)))) {
            detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
        }
        const auto resume = detach_accept(awaitable);
        if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
        return false;
    }
    awaitable.m_registration_owner = controller->m_registration_owner_slot;
    if (awaitable.m_timer) {
        const auto timer_ref = awaitable.m_operation->add_physical_reference();
        if (!timer_ref) {
            if (awaitable.select_error(CompletionReason::kBackendError, IOError(kNotReady, EOVERFLOW))) {
                const auto resume = detach_accept(awaitable);
                if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
            }
            return false;
        }
        awaitable.m_timer_attached = true;
        awaitable.m_timer->bind(&awaitable, [](void* value) noexcept {
            static_cast<AcceptAwaitable*>(value)->timeout_on_owner();
        });
        // push() can synchronously notify an already expired timer. Keep the
        // suspend boundary closed until it returns: no resume may escape yet.
        awaitable.m_submitting = true;
        const bool added = awaitable.m_scheduler->add_timer(awaitable.m_timer);
        awaitable.m_submitting = false;
        if (awaitable.m_operation->state().completion_reason()) { return false; }
        if (!added) {
            if (awaitable.select_error(CompletionReason::kBackendError, IOError(kNotReady, ENOMEM))) {
                const auto resume = detach_accept(awaitable);
                if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
            }
            return false;
        }
    }
    return true;
}

std::expected<ResumeCapability, OperationError>
EpollReactor::detach_accept(AcceptAwaitable& awaitable) {
    auto* controller = awaitable.m_registration_owner ? *awaitable.m_registration_owner
                                                     : awaitable.m_controller;
    if (controller && controller->m_awaitable[IOController::READ] == &awaitable) {
        controller->remove_awaitable(ACCEPT);
        // Readiness 注册不持有 frame 地址。先提交 DEL，再令已拷贝事件的稳定
        // entry 失效；即使 DEL 失败，晚到事件也不能访问 operation/controller。
        // Do not flush unrelated registrations here: their error recovery may
        // resume inline and destroy this controller before close/detach returns.
        discard_pending_change(controller);
        const int updated = update_registration(controller, build_events(controller));
        if (updated < 0) {
            detail::store_backend_error(m_last_error_code, kNotReady, static_cast<uint32_t>(-updated));
        }
        if (controller->m_type == IOEventType::INVALID) {
            retire_registration_entry(controller);
        }
    }
    return awaitable.detach();
}

void EpollReactor::timeout_accept(AcceptAwaitable& awaitable) {
    if (!awaitable.select_error(CompletionReason::kTimedOut, IOError(kTimeout, 0))) { return; }
    const bool submitting = awaitable.m_submitting;
    auto resume = detach_accept(awaitable);
    if (!resume) {
        detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
        return;
    }
    if (!submitting) { std::move(*resume).resume(); }
}

void EpollReactor::stop_accepts() {
    m_accept_stopping = true;

    // PERF: O(n) 优化 - 预先收集所有待处理的 accept awaitable
    std::vector<AcceptAwaitable*> pending_accepts;
    for (const auto& [fd, entry] : m_registration_entries) {
        (void)fd;
        auto* controller = entry->controller;
        if (controller && (static_cast<uint32_t>(controller->m_type) & ACCEPT)) {
            auto* awaitable = controller->get_awaitable<AcceptAwaitable>();
            if (awaitable) {
                pending_accepts.push_back(awaitable);
            }
        }
    }

    // 统一处理所有 accept
    for (auto* pending : pending_accepts) {
        if (!pending->select_error(CompletionReason::kRuntimeStopped, IOError(kClosed, 0))) {
            detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
            continue;
        }
        auto resume = detach_accept(*pending);
        if (!resume) {
            detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
            continue;
        }
        std::move(*resume).resume();
        // 内联 C 恢复允许销毁/移动其他资源
    }
}

int EpollReactor::add_connect(IOController* controller) {
    auto* awaitable = controller->get_awaitable<ConnectAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return apply_events(controller, build_events(controller));
}

int EpollReactor::add_recv(IOController* controller) {
    auto* awaitable = controller->get_awaitable<RecvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return arm_persistent_read(controller);
}

int EpollReactor::add_send(IOController* controller) {
    auto* awaitable = controller->get_awaitable<SendAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return arm_persistent_write(controller);
}

int EpollReactor::add_readv(IOController* controller) {
    auto* awaitable = controller->get_awaitable<ReadvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return arm_persistent_read(controller);
}

int EpollReactor::add_writev(IOController* controller) {
    auto* awaitable = controller->get_awaitable<WritevAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return apply_events(controller, build_events(controller));
}

int EpollReactor::add_send_file(IOController* controller) {
    auto* awaitable = controller->get_awaitable<SendFileAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return apply_events(controller, build_events(controller));
}

int EpollReactor::add_close(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    const int fd = controller->m_handle.fd;
    discard_pending_change(controller);

    std::optional<ResumeCapability> accept_resume;
    if ((static_cast<uint32_t>(controller->m_type) & ACCEPT) != 0) {
        if (auto* awaitable = controller->get_awaitable<AcceptAwaitable>(); awaitable != nullptr) {
            if (awaitable->select_error(CompletionReason::kResourceClosed, IOError(kClosed, 0))) {
                auto resume = detach_accept(*awaitable);
                if (resume) { accept_resume.emplace(std::move(*resume)); }
                else { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
            }
        }
    }

    controller->m_type = IOEventType::INVALID;
    controller->m_awaitable[IOController::READ] = nullptr;
    controller->m_awaitable[IOController::WRITE] = nullptr;
    controller->m_sequence_owner[IOController::READ] = nullptr;
    controller->m_sequence_owner[IOController::WRITE] = nullptr;
    controller->m_persistent_events = 0;
    detail::clear_sequence_interest_mask(controller);
    controller->m_registered_events = 0;
    retire_registration_entry(controller);

    const int close_result = ::close(fd);
    const int close_error = close_result == 0 ? 0 : (errno == 0 ? -1 : -errno);
    controller->m_handle = GHandle::invalid();
    if (accept_resume) { std::move(*accept_resume).resume(); }
    return close_error;
}

int EpollReactor::add_file_read(IOController* controller) {
    return apply_events(controller, EPOLLIN | EPOLLET);
}

int EpollReactor::add_file_write(IOController* controller) {
    return apply_events(controller, EPOLLOUT | EPOLLET);
}

int EpollReactor::add_recv_from(IOController* controller) {
    auto* awaitable = controller->get_awaitable<RecvFromAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return apply_events(controller, build_events(controller));
}

int EpollReactor::add_send_to(IOController* controller) {
    auto* awaitable = controller->get_awaitable<SendToAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handle_complete(controller->m_handle)) {
        return kImmediateReady;
    }
    return apply_events(controller, build_events(controller));
}

int EpollReactor::add_file_watch(IOController* controller) {
    auto* awaitable = controller->get_awaitable<FileWatchAwaitable>();
    if (awaitable == nullptr) return -1;
    return apply_events(controller, build_events(controller));
}

int EpollReactor::add_sequence(IOController* controller) {
    if (controller == nullptr) {
        return -1;
    }
    const auto desired_mask = detail::sync_sequence_interest_mask(controller);
    if ((desired_mask & detail::sequence_slot_mask(IOController::READ)) != 0) {
        return arm_persistent_read(controller);
    }
    return apply_events(controller, build_events(controller));
}

int EpollReactor::remove(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }
    controller->m_persistent_events = 0;
    return apply_events(controller, EPOLLET);
}

int EpollReactor::process_sequence(IOEventType type, IOController* controller) {
    const uint32_t events = sequence_interest_to_epoll_events(detail::sequence_interest_mask(type));
    if (events == EPOLLET) {
        return -1;
    }
    return apply_events(controller, events);
}

void EpollReactor::sync_events(IOController* controller) {
    if (controller != nullptr && (static_cast<uint32_t>(controller->m_type) & SEQUENCE) != 0) {
        (void)detail::sync_sequence_interest_mask(controller);
    }
    const uint32_t events = build_events(controller);
    if (apply_events(controller, events) < 0) {
        detail::store_backend_error(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

void EpollReactor::poll(int timeout_ms, WakeCoordinator& wake_coordinator) {
    if (flush_pending_changes() < 0) {
        return;
    }

    const int nev = epoll_wait(m_epoll_fd, m_events.data(), m_max_events, timeout_ms);
    if (nev < 0) {
        if (errno == EINTR) {
            return;
        }
        detail::store_backend_error(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
        return;
    }

    for (int i = 0; i < nev; ++i) {
        struct epoll_event& ev = m_events[i];
        if (ev.data.ptr == nullptr) {
            uint64_t val = 0;
            while (read(m_event_fd, &val, sizeof(val)) > 0) {}
            wake_coordinator.cancel_pending_wake();
            continue;
        }

        if (ev.events & EPOLLERR) {
            ev.events |= (EPOLLIN | EPOLLOUT);
        }

        process_event(ev);
    }
}

void EpollReactor::process_event(struct epoll_event& ev) {
    auto* entry = static_cast<RegistrationEntry*>(ev.data.ptr);
    auto* controller = entry ? entry->controller : nullptr;
    if (!controller ||
        controller->m_type == IOEventType::INVALID ||
        controller->m_handle == GHandle::invalid()) {
        return;
    }

    const uint32_t t = static_cast<uint32_t>(controller->m_type);
    const auto complete_one_shot = [this, controller](auto* awaitable,
                                                      IOEventType event_type) -> bool {
        if (awaitable == nullptr || !awaitable->handle_complete(controller->m_handle)) {
            return false;
        }

        Waker waker = awaitable->m_waker;
        // 完成派发时刻先裁决超时竞争再唤醒，防止恢复排队延迟误判超时。
        awaitable->cancel_bound_timeout_timer();
        controller->remove_awaitable(event_type);
        sync_events(controller);
        (void)flush_pending_changes();
        waker.wake_up();
        // A C coroutine may resume inline and destroy controller before this
        // dispatch returns.  No code below may dereference the old pointer.
        return true;
    };

    if (ev.events & EPOLLIN) {
        if (t & ACCEPT) {
            auto* awaitable = controller->get_awaitable<AcceptAwaitable>();
            if (awaitable && awaitable->select_ready(controller->m_handle)) {
                auto resume = detach_accept(*awaitable);
                if (!resume) {
                    detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
                    return;
                }
                std::move(*resume).resume();
            }
            return; // 恢复可能内联销毁 controller。
        } else if (t & RECV) {
            (void)complete_one_shot(controller->get_awaitable<RecvAwaitable>(), RECV);
        } else if (t & READV) {
            (void)complete_one_shot(controller->get_awaitable<ReadvAwaitable>(), READV);
        } else if (t & RECVFROM) {
            (void)complete_one_shot(controller->get_awaitable<RecvFromAwaitable>(), RECVFROM);
        } else if (t & FILEREAD) {
            auto* aio_awaitable =
                static_cast<galay::async::AioCommitAwaitable*>(controller->m_awaitable[IOController::READ]);
            if (aio_awaitable) {
                bool should_wake = false;
                uint64_t completed = 0;
                const ssize_t n = read(controller->m_handle.fd, &completed, sizeof(completed));
                if (n == static_cast<ssize_t>(sizeof(completed)) && completed > 0) {
                    const size_t expected_events = aio_awaitable->m_pending_count;
                    aio_awaitable->m_results.reserve(expected_events);

                    while (aio_awaitable->m_results.size() < expected_events) {
                        const size_t remaining = expected_events - aio_awaitable->m_results.size();
                        std::vector<struct io_event> events(remaining);
                        timespec timeout{0, 0};
                        const int num_events = io_getevents(aio_awaitable->m_aio_ctx,
                                                            0,
                                                            static_cast<long>(events.size()),
                                                            events.data(),
                                                            &timeout);
                        if (num_events < 0) {
                            aio_awaitable->m_result = std::unexpected(
                                IOError(kReadFailed, static_cast<uint32_t>(-num_events)));
                            should_wake = true;
                            break;
                        }
                        if (num_events == 0) {
                            break;
                        }
                        for (int i = 0; i < num_events; ++i) {
                            aio_awaitable->m_results.push_back(events[static_cast<size_t>(i)].res);
                        }
                    }

                    if (aio_awaitable->m_results.size() == expected_events) {
                        aio_awaitable->m_result = std::move(aio_awaitable->m_results);
                        should_wake = true;
                    }
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    return;
                } else {
                    aio_awaitable->m_result = std::unexpected(IOError(kReadFailed, errno));
                    should_wake = true;
                }

                if (!should_wake) {
                    return;
                }
                controller->remove_awaitable(FILEREAD);
                sync_events(controller);
                (void)flush_pending_changes();
                aio_awaitable->m_waker.wake_up();
                return;
            }
        } else if (t & FILEWATCH) {
            auto* awaitable = controller->get_awaitable<FileWatchAwaitable>();
            if (awaitable) {
                bool completed = false;
                std::expected<FileWatchResult, IOError> first_result =
                    std::unexpected(IOError(kReadFailed, 0));
                while (true) {
                    const ssize_t len =
                        read(controller->m_handle.fd, awaitable->m_buffer, awaitable->m_buffer_size);
                    if (len > 0) {
                        auto parsed = io::parse_inotify_events(awaitable->m_buffer,
                                                             static_cast<size_t>(len),
                                                             awaitable->m_ready_events);
                        if (!parsed) {
                            if (!completed) {
                                first_result = std::unexpected(parsed.error());
                                completed = true;
                            }
                            break;
                        }
                        if (!completed) {
                            first_result = std::move(parsed);
                            completed = true;
                        } else if (awaitable->m_ready_events != nullptr) {
                            awaitable->m_ready_events->push_back(std::move(parsed.value()));
                        }
                        continue;
                    }
                    if (len == 0) {
                        if (!completed) {
                            first_result = std::unexpected(IOError(kReadFailed, 0));
                            completed = true;
                        }
                        break;
                    }

                    const int saved_errno = errno;
                    if (saved_errno == EINTR) {
                        continue;
                    }
                    if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                        break;
                    }
                    if (!completed) {
                        first_result = std::unexpected(
                            IOError(kReadFailed, static_cast<uint32_t>(saved_errno)));
                        completed = true;
                    }
                    break;
                }

                if (!completed) {
                    return;
                }
                awaitable->m_result = std::move(first_result);
                // FILEWATCH 有独立的完成路径；和 one-shot IO 一样，在发布唤醒
                // 前先仲裁超时。
                awaitable->cancel_bound_timeout_timer();
                controller->remove_awaitable(FILEWATCH);
                sync_events(controller);
                (void)flush_pending_changes();
                awaitable->m_waker.wake_up();
                return;
            }
        }
    }

    // 同一 fd 的 EPOLLIN|EPOLLOUT 会被 epoll 合并进一个事件；读侧完成后
    // 必须继续分发写侧就绪位，否则双向并发收发的 send 会丢失 EPOLLOUT
    // 边沿而永久等待。当前 reactor 的 awaitable 均为 C++ 协程（wake_up 只入
    // 队不内联恢复），但仍复查 controller 有效性以防御未来的内联恢复路径。
    if (entry->controller != controller ||
        controller->m_handle == GHandle::invalid() ||
        controller->m_type == IOEventType::INVALID) {
        return;
    }
    const uint32_t after_read_type = static_cast<uint32_t>(controller->m_type);
    if (ev.events & EPOLLOUT) {
        if (after_read_type & CONNECT) {
            (void)complete_one_shot(controller->get_awaitable<ConnectAwaitable>(), CONNECT);
        } else if (after_read_type & SEND) {
            (void)complete_one_shot(controller->get_awaitable<SendAwaitable>(), SEND);
        } else if (after_read_type & WRITEV) {
            (void)complete_one_shot(controller->get_awaitable<WritevAwaitable>(), WRITEV);
        } else if (after_read_type & SENDTO) {
            (void)complete_one_shot(controller->get_awaitable<SendToAwaitable>(), SENDTO);
        } else if (after_read_type & FILEWRITE) {
            (void)complete_one_shot(controller->get_awaitable<FileWriteAwaitable>(), FILEWRITE);
        } else if (after_read_type & SENDFILE) {
            (void)complete_one_shot(controller->get_awaitable<SendFileAwaitable>(), SENDFILE);
        }
    }

    // 普通写和 read-only sequence 可以分别占用同一 controller 的两个域。
    // 写侧完成后仍需消费本次合并事件的 EPOLLIN，但先防御性地确认
    // completion 没有通过内联恢复使 controller 失效。
    if (entry->controller != controller ||
        controller->m_handle == GHandle::invalid() ||
        controller->m_type == IOEventType::INVALID) {
        return;
    }

    if (static_cast<uint32_t>(controller->m_type) & SEQUENCE) {
        const auto dispatch_owner = [this, controller](SequenceAwaitableBase* owner) -> bool {
            if (owner == nullptr) {
                return false;
            }

            const auto progress = owner->on_active_event(controller->m_handle);
            if (progress == SequenceProgress::kCompleted) {
                owner->on_completed();
                (void)detail::sync_sequence_interest_mask(controller);
                sync_events(controller);
                (void)flush_pending_changes();
                owner->m_waker.wake_up();
                return true;
            }

            const int ret = add_sequence(controller);
            if (ret == kImmediateReady) {
                owner->on_completed();
                (void)detail::sync_sequence_interest_mask(controller);
                sync_events(controller);
                (void)flush_pending_changes();
                owner->m_waker.wake_up();
                return true;
            } else if (ret < 0) {
                const uint32_t sys = (ret != -1)
                    ? static_cast<uint32_t>(-ret)
                    : static_cast<uint32_t>(errno);
                detail::store_backend_error(m_last_error_code, kNotReady, sys);
                owner->on_completed();
                (void)detail::sync_sequence_interest_mask(controller);
                sync_events(controller);
                (void)flush_pending_changes();
                owner->m_waker.wake_up();
                return true;
            }
            return false;
        };

        SequenceAwaitableBase* dispatched = nullptr;
        if ((ev.events & EPOLLIN) != 0) {
            auto* owner = controller->m_sequence_owner[IOController::READ];
            if (owner != nullptr && owner->waits_on(IOController::READ)) {
                // 同一 fd 的 EPOLLIN|EPOLLOUT 会被 epoll 合并进一个事件；
                // 读侧完成后必须继续分发写侧就绪位，否则双向并发收发会
                // 持续丢失 EPOLLOUT 边沿（sequence owner 均为 C++ 协程，
                // wake_up 不会内联销毁 controller，跨侧继续分发是安全的）。
                if (!dispatch_owner(owner)) {
                    dispatched = owner;
                }
            }
        }
        if ((ev.events & EPOLLOUT) != 0) {
            auto* owner = controller->m_sequence_owner[IOController::WRITE];
            if (owner != nullptr &&
                owner != dispatched &&
                owner->waits_on(IOController::WRITE)) {
                if (dispatch_owner(owner)) {
                    return;
                }
            }
        }
    }

    sync_events(controller);
}

}  // namespace galay::kernel

#endif  // USE_EPOLL
