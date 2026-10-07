/**
 * @file uring_reactor.cc
 * @brief Linux io_uring reactor 实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 Linux io_uring 实现 IO 事件注册、multishot accept/recv/recvmsg
 * （配合 provided buffer ring）、send_zc 门控、sequence SQE 提交和 CQE 处理。
 */

#include "uring_reactor.h"

#ifdef USE_IOURING

#include "awaitable.h"

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace galay::kernel {

namespace {

constexpr int kImmediateReady = 1;
constexpr int kSendNoSignalFlag =
#ifdef MSG_NOSIGNAL
    MSG_NOSIGNAL;
#elif defined(__linux__)
    0x4000;
#else
    0;
#endif

inline auto wake_user_data() -> void* {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

inline int wait_for_io_uring_completion(struct io_uring* ring,
                                    struct io_uring_cqe** cqe,
                                    struct __kernel_timespec* timeout) {
    if (io_uring_sq_ready(ring) == 0) {
        return io_uring_wait_cqe_timeout(ring, cqe, timeout);
    }

    const int ret = io_uring_submit_and_wait_timeout(ring, cqe, 1, timeout, nullptr);
    if (ret == -EBUSY) {
        return io_uring_wait_cqe_timeout(ring, cqe, timeout);
    }
    return ret;
}

inline auto negative_ret_or_errno(int ret) -> uint32_t {
    return (ret < 0 && ret != -1)
        ? static_cast<uint32_t>(-ret)
        : static_cast<uint32_t>(errno);
}

inline auto system_code_from_error(const IOError& error) -> uint32_t {
    return static_cast<uint32_t>(error.code() >> 32);
}

inline auto io_error_code_from_error(const IOError& error) -> IOErrorCode {
    return static_cast<IOErrorCode>(error.code() & 0xffffffffu);
}

template <typename Awaitable>
requires requires(Awaitable& awaitable) {
    { awaitable.cancel_bound_timeout_timer() } noexcept;
    { awaitable.m_waker.wake_up() } noexcept;
}
inline void complete_and_wake(Awaitable* awaitable) noexcept
{
    awaitable->cancel_bound_timeout_timer();
    awaitable->m_waker.wake_up();
}

inline bool resolve_sequence_slot(IOEventType type, IOController::Index& slot) {
    if (detail::sequence_event_uses_slot(type, IOController::READ)) {
        slot = IOController::READ;
        return true;
    }
    if (detail::sequence_event_uses_slot(type, IOController::WRITE)) {
        slot = IOController::WRITE;
        return true;
    }
    return false;
}

#if GALAY_HAS_IO_URING_RECVMSG_MULTISHOT
inline bool kernel_at_least(unsigned required_major, unsigned required_minor) noexcept
{
    utsname info{};
    if (::uname(&info) != 0) {
        return false;
    }

    const std::string_view release(info.release);
    const size_t major_end = release.find('.');
    if (major_end == std::string_view::npos) {
        return false;
    }
    const size_t minor_end = release.find('.', major_end + 1);
    const size_t minor_length = (minor_end == std::string_view::npos ? release.size() : minor_end) -
                                (major_end + 1);

    unsigned major = 0;
    unsigned minor = 0;
    const auto major_result = std::from_chars(release.data(), release.data() + major_end, major);
    const auto minor_result = std::from_chars(release.data() + major_end + 1,
                                              release.data() + major_end + 1 + minor_length,
                                              minor);
    if (major_result.ec != std::errc{} || minor_result.ec != std::errc{}) {
        return false;
    }
    return major > required_major ||
           (major == required_major && minor >= required_minor);
}
#endif

struct HandleRecycleGuard {
    SqeRequestHandle* handle = nullptr;

    ~HandleRecycleGuard() {
        if (handle != nullptr) {
            handle->recycle();
        }
    }
};

struct RecvBufferPool {
    RecvBufferPool(struct io_uring* target_ring,
                   uint16_t entries,
                   uint16_t bgid,
                   size_t buf_size)
        : ring(target_ring)
        , ring_entries(entries)
        , buffer_group(bgid)
        , buffer_size(buf_size)
        , mask(io_uring_buf_ring_mask(entries)) {
    }

    std::expected<void, IOError> initialize() {
        int ret = 0;
        buf_ring = io_uring_setup_buf_ring(ring, ring_entries, buffer_group, 0, &ret);
        if (buf_ring == nullptr || ret < 0) {
            const uint32_t system_code = ret < 0
                ? static_cast<uint32_t>(-ret)
                : static_cast<uint32_t>(errno);
            return std::unexpected(IOError(kOpenFailed, system_code));
        }

        io_uring_buf_ring_init(buf_ring);
        buffers.reserve(ring_entries);
        for (uint16_t bid = 0; bid < ring_entries; ++bid) {
            auto storage = std::make_unique<char[]>(buffer_size);
            io_uring_buf_ring_add(buf_ring,
                                  storage.get(),
                                  static_cast<unsigned>(buffer_size),
                                  bid,
                                  mask,
                                  bid);
            buffers.push_back(std::move(storage));
        }
        io_uring_buf_ring_advance(buf_ring, ring_entries);
        return {};
    }

    ~RecvBufferPool() = default;

    char* data(uint16_t bid) const noexcept {
        if (bid >= buffers.size()) {
            return nullptr;
        }
        return buffers[bid].get();
    }

    void recycle(uint16_t bid) noexcept {
        if (!active || buf_ring == nullptr || bid >= buffers.size()) {
            return;
        }
        io_uring_buf_ring_add(buf_ring,
                              buffers[bid].get(),
                              static_cast<unsigned>(buffer_size),
                              bid,
                              mask,
                              0);
        io_uring_buf_ring_advance(buf_ring, 1);
    }

    void shutdown() noexcept {
        active = false;
        if (buf_ring != nullptr) {
            (void)io_uring_free_buf_ring(ring, buf_ring, ring_entries, buffer_group);
            buf_ring = nullptr;
        }
    }

    struct io_uring* ring = nullptr;
    struct io_uring_buf_ring* buf_ring = nullptr;
    uint16_t ring_entries = 0;
    uint16_t buffer_group = 0;
    size_t buffer_size = 0;
    int mask = 0;
    bool active = true;
    std::vector<std::unique_ptr<char[]>> buffers;
};

inline void recycle_recv_buffer(const std::shared_ptr<void>& owner, uint16_t bid) noexcept {
    if (!owner) {
        return;
    }
    static_cast<RecvBufferPool*>(owner.get())->recycle(bid);
}

inline auto recv_buffer_pool(const std::shared_ptr<void>& owner) -> RecvBufferPool* {
    return static_cast<RecvBufferPool*>(owner.get());
}

inline auto cqe_buffer_id(const struct io_uring_cqe* cqe) -> uint16_t {
    return static_cast<uint16_t>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
}

inline bool try_immediate_readv(int fd, ReadvIOContext* ctx, int& res) {
    if (ctx == nullptr || ctx->m_iovecs.empty()) {
        res = 0;
        return true;
    }

    ssize_t n = 0;
    if (ctx->m_iovecs.size() == 1) {
        const auto& iov = ctx->m_iovecs[0];
        n = ::recv(fd, iov.iov_base, iov.iov_len, MSG_DONTWAIT);
    } else {
        msghdr msg = ctx->m_msg;
        n = ::recvmsg(fd, &msg, MSG_DONTWAIT);
    }

    if (n >= 0) {
        res = static_cast<int>(n);
        return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return false;
    }
    res = -errno;
    return true;
}

}  // namespace

IOUringReactor::IOUringReactor(int queue_depth, std::atomic<uint64_t>& last_error_code)
    : m_queue_depth(queue_depth)
    , m_last_error_code(last_error_code) {}

std::expected<void, IOError> IOUringReactor::start()
{
    m_accept_stopping = false;
    if (m_ring_initialized) {
        return {};
    }

    m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_event_fd == -1) {
        detail::store_backend_error(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_COOP_TASKRUN;
    params.sq_thread_idle = 1000;

    int init_result = io_uring_queue_init_params(m_queue_depth, &m_ring, &params);
    if (init_result < 0) {
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_COOP_TASKRUN;
        init_result = io_uring_queue_init_params(m_queue_depth, &m_ring, &params);
        if (init_result < 0) {
            std::memset(&params, 0, sizeof(params));
            init_result = io_uring_queue_init_params(m_queue_depth, &m_ring, &params);
            if (init_result < 0) {
                const uint32_t system_code = static_cast<uint32_t>(-init_result);
                close(m_event_fd);
                m_event_fd = -1;
                detail::store_backend_error(m_last_error_code, kOpenFailed, system_code);
                return std::unexpected(IOError(kOpenFailed, system_code));
            }
        }
    }
    m_ring_initialized = true;

    auto recv_pool = std::make_shared<RecvBufferPool>(&m_ring,
                                                      kRecvBufferCount,
                                                      kRecvBufferGroup,
                                                      kRecvBufferSize);
    auto recv_pool_ready = recv_pool->initialize();
    if (!recv_pool_ready) {
        const auto error = recv_pool_ready.error();
        detail::store_backend_error(
            m_last_error_code,
            io_error_code_from_error(error),
            system_code_from_error(error));
        io_uring_queue_exit(&m_ring);
        m_ring_initialized = false;
        close(m_event_fd);
        m_event_fd = -1;
        return std::unexpected(error);
    }
    m_recv_buffer_pool = std::static_pointer_cast<void>(std::move(recv_pool));

    bool recvmsg_opcode_supported = false;
    if (io_uring_probe* probe = io_uring_get_probe_ring(&m_ring); probe != nullptr) {
        m_send_zc_supported = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC) != 0;
        recvmsg_opcode_supported = io_uring_opcode_supported(probe, IORING_OP_RECVMSG) != 0;
        io_uring_free_probe(probe);
    }

#if GALAY_HAS_IO_URING_RECVMSG_MULTISHOT
    m_recvmsg_multishot_supported = recvmsg_opcode_supported && kernel_at_least(6, 0);
#else
    (void)recvmsg_opcode_supported;
#endif
    return {};
}

IOUringReactor::~IOUringReactor() {
    if (m_recvfrom_buffer_pool) {
        recv_buffer_pool(m_recvfrom_buffer_pool)->shutdown();
    }
    if (m_recv_buffer_pool) {
        recv_buffer_pool(m_recv_buffer_pool)->shutdown();
    }
    if (m_ring_initialized) {
        io_uring_queue_exit(&m_ring);
    }
    // A persistent handle owns a shared_ptr back to its arena. No CQE can be
    // delivered after queue_exit(), so this is the first safe point to break
    // that cycle for requests cancelled by shutdown without a terminal CQE.
    for (auto& [state, registration] : m_accept_registrations) {
        (void)state;  // 避免未使用警告
        if (registration.handle != nullptr && registration.handle->arena) {
            registration.handle->recycle();
        }
    }
    m_accept_registrations.clear();
    if (m_event_fd != -1) {
        close(m_event_fd);
    }
}

void IOUringReactor::notify() {
    uint64_t val = 1;
    if (write(m_event_fd, &val, sizeof(val)) < 0) {
        detail::store_backend_error(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

GHandle IOUringReactor::get_handle() const {
    return {m_event_fd};
}

bool IOUringReactor::should_use_send_zc(size_t length) const noexcept {
    return m_send_zc_supported && length >= kSendZcThreshold;
}

void IOUringReactor::prepare_send_sqe(struct io_uring_sqe* sqe,
                                    SqeRequestHandle* handle,
                                    int fd,
                                    const void* buffer,
                                    size_t length,
                                    int flags) {
    if (handle != nullptr) {
        handle->notify_expected = false;
        handle->notify_received = false;
        handle->result_completed = false;
    }

    const int send_flags = flags | kSendNoSignalFlag;
    if (should_use_send_zc(length)) {
        io_uring_prep_send_zc(sqe,
                              fd,
                              buffer,
                              length,
                              send_flags,
                              IORING_SEND_ZC_REPORT_USAGE);
        if (handle != nullptr) {
            handle->notify_expected = true;
        }
        return;
    }

    io_uring_prep_send(sqe, fd, buffer, length, send_flags);
}

bool IOUringReactor::submit_accept(AcceptAwaitable& awaitable, Waker&& waker) {
    auto* controller = awaitable.m_controller;
    const bool valid = controller && controller->m_handle != GHandle::invalid();
    const OperationKey key{valid ? static_cast<uint32_t>(controller->m_handle.fd) : 0,
                           m_next_accept_generation};
    // emplace 返回内部可写别名；这里只构造存储，不向 adapter 暴露该别名。
    (void)awaitable.m_operation.emplace(key, std::move(waker));
    const auto fail = [&](IOError error) {
        if (awaitable.select_error(CompletionReason::kBackendError, error)) {
            const auto resume = detach_accept(awaitable); // 同步继续，不调用恢复回调。
            if (!resume) { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
        }
        return false;
    };
    if (!valid || m_accept_stopping) { return fail(IOError(kClosed, 0)); }
    if (!m_ring_initialized) { return fail(IOError(kNotReady, EBADF)); }
    if (m_next_accept_generation == 0) { return fail(IOError(kNotReady, EOVERFLOW)); }
    ++m_next_accept_generation; // 耗尽后拒绝新 key；不改变持久 SQE 的 generation。
    if (controller->m_awaitable[IOController::READ] ||
        controller->m_sequence_owner[IOController::READ]) {
        return fail(IOError(kNotReady, EBUSY));
    }
    if (!awaitable.m_operation->mark_submitted()) { return fail(IOError(kNotReady, EINVAL)); }
    const auto retained = awaitable.m_operation->add_physical_reference();
    if (!retained) { return fail(IOError(kNotReady, EOVERFLOW)); }
    awaitable.m_sqe_type = ACCEPT;
    if (!controller->fill_awaitable(ACCEPT, &awaitable)) { return fail(IOError(kNotReady, EINVAL)); }
    awaitable.m_registration_state = controller->m_sqe_state[IOController::READ];
    const int result = add_accept(controller);
    if (result < 0) { return fail(IOError(kAcceptFailed, negative_ret_or_errno(result))); }
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
    if (awaitable.m_timer) {
        const auto retained_timer = awaitable.m_operation->add_physical_reference();
        if (!retained_timer) { return fail(IOError(kNotReady, EOVERFLOW)); }
        awaitable.m_timer_attached = true;
        awaitable.m_timer->bind(&awaitable, [](void* value) noexcept {
            static_cast<AcceptAwaitable*>(value)->timeout_on_owner();
        });
        // 时间轮 push 可同步到期；此时只允许 await_suspend 返回 false，不能内联恢复。
        awaitable.m_submitting = true;
        const bool added = awaitable.m_scheduler->add_timer(awaitable.m_timer);
        awaitable.m_submitting = false;
        if (awaitable.m_operation->state().completion_reason()) { return false; }
        if (!added) { return fail(IOError(kNotReady, ENOMEM)); }
    }
    return true;
}

std::expected<ResumeCapability, OperationError>
IOUringReactor::detach_accept(AcceptAwaitable& awaitable) {
    auto* controller = awaitable.m_registration_state
        ? awaitable.m_registration_state->owner.load(std::memory_order_acquire)
        : awaitable.m_controller;
    if (controller && controller->m_awaitable[IOController::READ] == &awaitable) {
        // 只解除当前 frame 的入口。持久 SQE 无 frame 指针，后续 fd 仍归资源缓存。
        controller->remove_awaitable(ACCEPT);
    }
    return awaitable.detach();
}

void IOUringReactor::timeout_accept(AcceptAwaitable& awaitable) {
    if (!awaitable.select_error(CompletionReason::kTimedOut, IOError(kTimeout, 0))) { return; }
    const bool submitting = awaitable.m_submitting;
    auto resume = detach_accept(awaitable);
    if (!resume) {
        detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
        return;
    }
    if (!submitting) { std::move(*resume).resume(); }
}

int IOUringReactor::add_accept(IOController* controller) {
    auto* awaitable = controller->get_awaitable<AcceptAwaitable>();
    if (awaitable == nullptr) { return -EINVAL; }
    if (const auto accepted = controller->take_accepted_handle()) {
        if (awaitable->select_ready(*accepted)) { return kImmediateReady; }
        return -EINVAL; // 已有 winner 不应再次发布同一个 awaiter。
    }
    if (controller->m_accept_multishot_armed) { return 0; }
    return submit_multishot_accept(controller);
}

int IOUringReactor::submit_multishot_accept(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -EINVAL;
    }

    // 每次重新挂载 multishot 请求都切换一次 request epoch，
    // 让旧 request 的晚到 CQE 无法误命中新 handle。
    controller->advance_sqe_generation(IOController::READ);
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    auto* state = controller->m_sqe_state[IOController::READ];
    if (state != nullptr) {
        // PERF: O(1) 查找和插入，使用 unordered_map
        auto& registration = m_accept_registrations[state];
        registration.arena = controller->m_sqe_handle_pool[IOController::READ];
        registration.state = state;
        registration.handle = handle;
    }

    io_uring_prep_multishot_accept(sqe,
                                   controller->m_handle.fd,
                                   nullptr,
                                   nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data(sqe, handle);
    handle->multishot_type = ACCEPT;
    handle->persistent = true;
    controller->m_accept_multishot_handle = handle;
    controller->m_accept_multishot_armed = true;
    return 0;
}

void IOUringReactor::stop_accepts() {
    m_accept_stopping = true;
    // PERF: O(n) 优化 - 使用 unordered_map 迭代
    for (auto& [state_key, registration] : m_accept_registrations) {
        auto* state = registration.state;
        if (state == nullptr) {
            continue;
        }
        auto* controller = state->owner.load(std::memory_order_acquire);
        if (controller == nullptr ||
            (static_cast<uint32_t>(controller->m_type) & ACCEPT) == 0) {
            continue;
        }
        // add_close() is the owner-side logical completion boundary. It
        // invalidates generation before waking the task; late accept CQEs
        // close undelivered fds and retain the handle until terminal. Ring
        // cancellation/drain remain a separate physical-shutdown gate.
        const int closed = add_close(controller);
        if (closed < 0) {
            detail::store_backend_error(m_last_error_code, kDisconnectError, negative_ret_or_errno(closed));
        }
    }
}

int IOUringReactor::add_connect(IOController* controller) {
    auto* awaitable = controller->get_awaitable<ConnectAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::WRITE);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    io_uring_prep_connect(sqe,
                          controller->m_handle.fd,
                          awaitable->m_host.sock_addr(),
                          *awaitable->m_host.addr_len());
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_recv(IOController* controller) {
    auto* awaitable = controller->get_awaitable<RecvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (controller->try_consume_ready_recv(awaitable->m_buffer,
                                        awaitable->m_length,
                                        awaitable->m_result)) {
        return kImmediateReady;
    }
    if (controller->m_recv_multishot_armed) {
        return 0;
    }
    return submit_multishot_recv(controller);
}

int IOUringReactor::submit_multishot_recv(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -EINVAL;
    }
    if (!m_recv_buffer_pool) {
        return -EINVAL;
    }

    // awaitable 切换不应使持久 recv handle 失效，但每次真正重挂
    // multishot recv SQE 时必须推进 request epoch 以隔离旧 CQE。
    controller->advance_sqe_generation(IOController::READ);
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    io_uring_prep_recv_multishot(sqe, controller->m_handle.fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kRecvBufferGroup;
    io_uring_sqe_set_data(sqe, handle);
    handle->multishot_type = RECV;
    handle->persistent = true;
    controller->m_recv_multishot_handle = handle;
    controller->m_recv_multishot_armed = true;
    return 0;
}

int IOUringReactor::add_send(IOController* controller) {
    auto* awaitable = controller->get_awaitable<SendAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::WRITE);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    prepare_send_sqe(sqe,
                   handle,
                   controller->m_handle.fd,
                   awaitable->m_buffer,
                   awaitable->m_length,
                   0);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_readv(IOController* controller) {
    auto* awaitable = controller->get_awaitable<ReadvAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    if (awaitable->m_iovecs.size() == 1) {
        const auto& iov = awaitable->m_iovecs[0];
        io_uring_prep_recv(sqe,
                           controller->m_handle.fd,
                           iov.iov_base,
                           static_cast<unsigned>(iov.iov_len),
                           0);
    } else {
        io_uring_prep_recvmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, 0);
    }
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_writev(IOController* controller) {
    auto* awaitable = controller->get_awaitable<WritevAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::WRITE);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    if (awaitable->m_iovecs.size() == 1) {
        const auto& iov = awaitable->m_iovecs[0];
        io_uring_prep_send(sqe,
                           controller->m_handle.fd,
                           iov.iov_base,
                           static_cast<unsigned>(iov.iov_len),
                           kSendNoSignalFlag);
    } else {
        io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, kSendNoSignalFlag);
    }
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_send_file(IOController* controller) {
    auto* awaitable = controller->get_awaitable<SendFileAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::WRITE);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    io_uring_prep_poll_add(sqe, controller->m_handle.fd, POLLOUT);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_close(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    const int fd = controller->m_handle.fd;

    std::optional<ResumeCapability> accept_resume;
    if ((static_cast<uint32_t>(controller->m_type) & ACCEPT) != 0) {
        if (auto* awaitable = controller->get_awaitable<AcceptAwaitable>();
            awaitable && awaitable->select_error(m_accept_stopping
                ? CompletionReason::kRuntimeStopped : CompletionReason::kResourceClosed,
                IOError(kClosed, 0))) {
            auto resume = detach_accept(*awaitable);
            // 不使用 emplace 的可写别名；仅在资源记账完成后消费局部恢复权。
            if (resume) { (void)accept_resume.emplace(std::move(*resume)); }
            else { detail::store_backend_error(m_last_error_code, kNotReady, EINVAL); }
        }
    }

    struct io_uring_sqe* cancel_sqe = io_uring_get_sqe(&m_ring);
    if (cancel_sqe) {
        io_uring_prep_cancel_fd(cancel_sqe, fd, 0);
        io_uring_sqe_set_data(cancel_sqe, nullptr);
    }

    int result = 0;
    struct io_uring_sqe* close_sqe = io_uring_get_sqe(&m_ring);
    if (!close_sqe) {
        if (::close(fd) != 0) { result = -errno; }
    } else {
        io_uring_prep_close(close_sqe, fd);
        io_uring_sqe_set_data(close_sqe, nullptr);
    }

    controller->m_type = IOEventType::INVALID;
    controller->m_awaitable[IOController::READ] = nullptr;
    controller->m_awaitable[IOController::WRITE] = nullptr;
    controller->invalidate_sqe_requests();

    if (accept_resume) {
        std::move(*accept_resume).resume();
        return result; // 恢复回调之后不得访问 controller/awaiter。
    }

    controller->m_handle = GHandle::invalid();
    return result;
}

int IOUringReactor::add_file_read(IOController* controller) {
    auto* awaitable = controller->get_awaitable<FileReadAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    io_uring_prep_read(sqe,
                       controller->m_handle.fd,
                       awaitable->m_buffer,
                       awaitable->m_length,
                       awaitable->m_offset);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_file_write(IOController* controller) {
    auto* awaitable = controller->get_awaitable<FileWriteAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::WRITE);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    io_uring_prep_write(sqe,
                        controller->m_handle.fd,
                        awaitable->m_buffer,
                        awaitable->m_length,
                        awaitable->m_offset);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_recv_from(IOController* controller) {
    auto* awaitable = controller->get_awaitable<RecvFromAwaitable>();
    if (awaitable == nullptr) return -1;
    if (controller->try_consume_ready_recv_from(awaitable->m_buffer,
                                            awaitable->m_length,
                                            awaitable->m_from,
                                            awaitable->m_result)) {
        return kImmediateReady;
    }
    if (m_recvmsg_multishot_supported) {
        if (!m_recvfrom_buffer_pool) {
            auto pool_ready = initialize_recv_from_buffer_pool();
            if (!pool_ready) {
                const auto error = pool_ready.error();
                detail::store_backend_error(
                    m_last_error_code,
                    io_error_code_from_error(error),
                    system_code_from_error(error));
                m_recvmsg_multishot_supported = false;
                m_recvmsg_multishot_confirmed = false;
            }
        }
        if (controller->m_recvfrom_multishot_armed) {
            return 0;
        }
        if (m_recvmsg_multishot_supported) {
            return submit_multishot_recv_from(controller);
        }
    }
    return add_recv_from_one_shot(controller, awaitable);
}

std::expected<void, IOError> IOUringReactor::initialize_recv_from_buffer_pool()
{
    auto recvfrom_pool = std::make_shared<RecvBufferPool>(&m_ring,
                                                          kRecvFromBufferCount,
                                                          kRecvFromBufferGroup,
                                                          kRecvFromBufferSize);
    auto pool_ready = recvfrom_pool->initialize();
    if (!pool_ready) {
        return std::unexpected(pool_ready.error());
    }
    m_recvfrom_buffer_pool = std::static_pointer_cast<void>(std::move(recvfrom_pool));
    return {};
}

int IOUringReactor::submit_multishot_recv_from(IOController* controller)
{
#if !GALAY_HAS_IO_URING_RECVMSG_MULTISHOT
    (void)controller;
    return -EOPNOTSUPP;
#else
    if (controller == nullptr || controller->m_handle == GHandle::invalid() ||
        !m_recvfrom_buffer_pool) {
        return -EINVAL;
    }

    controller->advance_sqe_generation(IOController::READ);
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    auto* message = handle->arena != nullptr ? handle->arena->recv_from_message() : nullptr;
    if (message == nullptr) {
        handle->recycle();
        return -EINVAL;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    std::memset(message, 0, sizeof(*message));
    message->msg_namelen = sizeof(sockaddr_storage);

    io_uring_prep_recvmsg_multishot(sqe, controller->m_handle.fd, message, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = kRecvFromBufferGroup;
    io_uring_sqe_set_data(sqe, handle);
    handle->multishot_type = RECVFROM;
    handle->persistent = true;
    controller->m_recvfrom_multishot_handle = handle;
    controller->m_recvfrom_multishot_armed = true;
    return 0;
#endif
}

int IOUringReactor::add_recv_from_one_shot(IOController* controller,
                                       RecvFromAwaitable* awaitable)
{
    if (controller == nullptr || awaitable == nullptr) {
        return -EINVAL;
    }
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    std::memset(&awaitable->m_msg, 0, sizeof(awaitable->m_msg));
    std::memset(&awaitable->m_addr, 0, sizeof(awaitable->m_addr));
    awaitable->m_iov.iov_base = awaitable->m_buffer;
    awaitable->m_iov.iov_len = awaitable->m_length;
    awaitable->m_msg.msg_iov = &awaitable->m_iov;
    awaitable->m_msg.msg_iovlen = 1;
    awaitable->m_msg.msg_name = &awaitable->m_addr;
    awaitable->m_msg.msg_namelen = sizeof(awaitable->m_addr);

    io_uring_prep_recvmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, 0);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_send_to(IOController* controller) {
    auto* awaitable = controller->get_awaitable<SendToAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::WRITE);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    std::memset(&awaitable->m_msg, 0, sizeof(awaitable->m_msg));
    awaitable->m_iov.iov_base = const_cast<char*>(awaitable->m_buffer);
    awaitable->m_iov.iov_len = awaitable->m_length;
    awaitable->m_msg.msg_iov = &awaitable->m_iov;
    awaitable->m_msg.msg_iovlen = 1;
    awaitable->m_msg.msg_name = const_cast<sockaddr*>(awaitable->m_to.sock_addr());
    awaitable->m_msg.msg_namelen = *awaitable->m_to.addr_len();

    io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &awaitable->m_msg, kSendNoSignalFlag);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_file_watch(IOController* controller) {
    auto* awaitable = controller->get_awaitable<FileWatchAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* handle = controller->make_sqe_request(IOController::READ);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    io_uring_prep_read(sqe,
                       controller->m_handle.fd,
                       awaitable->m_buffer,
                       awaitable->m_buffer_size,
                       0);
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::add_sequence(IOController* controller) {
    if (controller == nullptr) {
        return -1;
    }

    const auto submit_owner = [this, controller](SequenceAwaitableBase* owner) -> int {
        if (owner == nullptr) {
            return 0;
        }

        auto* task = owner->front();
        if (task == nullptr) {
            return 0;
        }
        if (task->context == nullptr) {
            return -EINVAL;
        }

        const IOEventType type = owner->resolve_task_event_type(*task);
        IOController::Index slot = IOController::READ;
        if (!resolve_sequence_slot(type, slot)) {
            return -EINVAL;
        }
        if (controller->m_awaitable[slot] == owner) {
            return 0;
        }
        if (controller->m_awaitable[slot] != nullptr) {
            return -EBUSY;
        }
        const int ret = submit_sequence_sqe(slot, type, task->context, controller, owner);
        if (ret == kImmediateReady) {
            owner->on_completed();
            owner->m_waker.wake_up();
            return 0;
        }
        return ret;
    };

    auto* read_owner = controller->m_sequence_owner[IOController::READ];
    if (const int ret = submit_owner(read_owner); ret != 0) {
        return ret;
    }

    auto* write_owner = controller->m_sequence_owner[IOController::WRITE];
    if (write_owner != read_owner) {
        if (const int ret = submit_owner(write_owner); ret != 0) {
            return ret;
        }
    }

    return 0;
}

int IOUringReactor::submit_sequence_sqe(IOController::Index slot,
                                      IOEventType type,
                                      IOContextBase* ctx,
                                      IOController* controller,
                                      SequenceAwaitableBase* owner) {
    if (type == READV) {
        // 这里的 Sequence READV 由就绪事件驱动；completion 由非阻塞 socket read 产生，
        // 因此分阶段推进 sequence 不会漏掉已就绪字节。
        auto* readv_ctx = static_cast<ReadvIOContext*>(ctx);
        int immediate_res = 0;
        if (try_immediate_readv(controller->m_handle.fd, readv_ctx, immediate_res)) {
            io_uring_cqe ready_cqe{};
            ready_cqe.res = immediate_res;
            const auto progress = owner->on_active_event(&ready_cqe, controller->m_handle);
            if (progress == SequenceProgress::kCompleted) {
                return kImmediateReady;
            }
            return add_sequence(controller);
        }
    }

    auto* handle = controller->make_sqe_request(slot);
    if (handle == nullptr) {
        return -ENOMEM;
    }

    auto* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        handle->recycle();
        return -EAGAIN;
    }

    switch (type) {
    case RECV: {
        auto* c = static_cast<RecvIOContext*>(ctx);
        io_uring_prep_recv(sqe, controller->m_handle.fd, c->m_buffer, c->m_length, 0);
        break;
    }
    case SEND: {
        auto* c = static_cast<SendIOContext*>(ctx);
        prepare_send_sqe(sqe, handle, controller->m_handle.fd, c->m_buffer, c->m_length, 0);
        break;
    }
    case ACCEPT: {
        auto* c = static_cast<AcceptIOContext*>(ctx);
        io_uring_prep_accept(sqe,
                             controller->m_handle.fd,
                             c->m_host->sock_addr(),
                             c->m_host->addr_len(),
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        break;
    }
    case CONNECT: {
        auto* c = static_cast<ConnectIOContext*>(ctx);
        io_uring_prep_connect(sqe,
                              controller->m_handle.fd,
                              c->m_host.sock_addr(),
                              *c->m_host.addr_len());
        break;
    }
    case READV: {
        io_uring_prep_poll_add(sqe, controller->m_handle.fd, POLLIN);
        break;
    }
    case WRITEV: {
        auto* c = static_cast<WritevIOContext*>(ctx);
        if (c->m_iovecs.size() == 1) {
            const auto& iov = c->m_iovecs[0];
            io_uring_prep_send(sqe,
                               controller->m_handle.fd,
                               iov.iov_base,
                               static_cast<unsigned>(iov.iov_len),
                               kSendNoSignalFlag);
        } else {
            io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &c->m_msg, kSendNoSignalFlag);
        }
        break;
    }
    case FILEREAD: {
        auto* c = static_cast<FileReadIOContext*>(ctx);
        io_uring_prep_read(sqe, controller->m_handle.fd, c->m_buffer, c->m_length, c->m_offset);
        break;
    }
    case FILEWRITE: {
        auto* c = static_cast<FileWriteIOContext*>(ctx);
        io_uring_prep_write(sqe, controller->m_handle.fd, c->m_buffer, c->m_length, c->m_offset);
        break;
    }
    case RECVFROM: {
        auto* c = static_cast<RecvFromIOContext*>(ctx);
        std::memset(&c->m_msg, 0, sizeof(c->m_msg));
        std::memset(&c->m_addr, 0, sizeof(c->m_addr));
        c->m_iov.iov_base = c->m_buffer;
        c->m_iov.iov_len = c->m_length;
        c->m_msg.msg_iov = &c->m_iov;
        c->m_msg.msg_iovlen = 1;
        c->m_msg.msg_name = &c->m_addr;
        c->m_msg.msg_namelen = sizeof(c->m_addr);
        io_uring_prep_recvmsg(sqe, controller->m_handle.fd, &c->m_msg, 0);
        break;
    }
    case SENDTO: {
        auto* c = static_cast<SendToIOContext*>(ctx);
        std::memset(&c->m_msg, 0, sizeof(c->m_msg));
        c->m_iov.iov_base = const_cast<char*>(c->m_buffer);
        c->m_iov.iov_len = c->m_length;
        c->m_msg.msg_iov = &c->m_iov;
        c->m_msg.msg_iovlen = 1;
        c->m_msg.msg_name = const_cast<sockaddr*>(c->m_to.sock_addr());
        c->m_msg.msg_namelen = *c->m_to.addr_len();
        io_uring_prep_sendmsg(sqe, controller->m_handle.fd, &c->m_msg, kSendNoSignalFlag);
        break;
    }
    case SENDFILE:
        io_uring_prep_poll_add(sqe, controller->m_handle.fd, POLLOUT);
        break;
    case FILEWATCH: {
        auto* c = static_cast<FileWatchIOContext*>(ctx);
        io_uring_prep_read(sqe, controller->m_handle.fd, c->m_buffer, c->m_buffer_size, 0);
        break;
    }
    default:
        handle->recycle();
        return -EINVAL;
    }

    owner->m_sqe_type = SEQUENCE;
    controller->m_awaitable[slot] = owner;
    io_uring_sqe_set_data(sqe, handle);
    return 0;
}

int IOUringReactor::remove(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        return -EAGAIN;
    }

    io_uring_prep_cancel_fd(sqe, controller->m_handle.fd, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    return 0;
}

void IOUringReactor::ensure_wake_read_armed() {
    if (m_wake_read_armed) {
        return;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        return;
    }

    io_uring_prep_read(sqe, m_event_fd, &m_eventfd_buf, sizeof(m_eventfd_buf), 0);
    io_uring_sqe_set_data(sqe, wake_user_data());
    m_wake_read_armed = true;
}

void IOUringReactor::poll(uint64_t timeout_ns, WakeCoordinator& wake_coordinator) {
    ensure_wake_read_armed();

    struct io_uring_cqe* cqe = nullptr;
    struct __kernel_timespec timeout;
    timeout.tv_sec = static_cast<__kernel_time64_t>(timeout_ns / 1000000000ULL);
    timeout.tv_nsec = timeout_ns % 1000000000ULL;

    const int ret = wait_for_io_uring_completion(&m_ring, &cqe, &timeout);
    if (ret < 0) {
        if (ret == -EINTR || ret == -ETIME) {
            return;
        }
        detail::store_backend_error(
            m_last_error_code, kNotReady, static_cast<uint32_t>(-ret));
        return;
    }

    unsigned head = 0;
    unsigned count = 0;
    bool wake_triggered = false;

    io_uring_for_each_cqe(&m_ring, head, cqe) {
        void* user_data = io_uring_cqe_get_data(cqe);
        if (user_data == wake_user_data()) {
            wake_triggered = true;
            m_wake_read_armed = false;
        } else if (user_data != nullptr) {
            process_completion(cqe);
        }
        ++count;
    }

    if (count > 0) {
        io_uring_cq_advance(&m_ring, count);
    }

    if (wake_triggered) {
        wake_coordinator.cancel_pending_wake();
        ensure_wake_read_armed();
    }
}

void IOUringReactor::process_completion(struct io_uring_cqe* cqe) {
    void* data = io_uring_cqe_get_data(cqe);
    if (!data) {
        return;
    }

    auto* handle = static_cast<SqeRequestHandle*>(data);
    HandleRecycleGuard recycle_guard{handle};
    const bool notification = (cqe->flags & IORING_CQE_F_NOTIF) != 0;
    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    if (notification) {
        handle->notify_received = true;
        if (!handle->result_completed) {
            recycle_guard.handle = nullptr;
        }
    } else if (handle->persistent) {
        if (more) {
            recycle_guard.handle = nullptr;
        } else {
            handle->persistent = false;
        }
    } else if (handle->notify_expected) {
        handle->result_completed = true;
        if (!handle->notify_received) {
            recycle_guard.handle = nullptr;
        }
    }
    auto* state = handle->state;
    auto* controller = state != nullptr &&
            state->generation.load(std::memory_order_acquire) == handle->generation
        ? state->owner.load(std::memory_order_acquire) : nullptr;
    if (!controller) {
        // A stale accept still transfers a process fd. Resolve its type from
        // the physical request, never from a replaced/freed awaitable. MORE
        // keeps this identity alive; only the original terminal CQE recycles it.
        if (!notification && handle->multishot_type == ACCEPT && cqe->res >= 0) {
            if (::close(cqe->res) != 0) {
                detail::store_backend_error(
                    m_last_error_code, kDisconnectError, static_cast<uint32_t>(errno));
            }
        }
        return;
    }

    if (notification) {
        return;
    }

    const auto slot = static_cast<IOController::Index>(handle->state->slot);
    auto* base = static_cast<AwaitableBase*>(controller->m_awaitable[slot]);
    if (!base) {
        if (slot == IOController::READ) {
            if (controller->m_recvfrom_multishot_armed) {
                process_recv_from_completion(controller, nullptr, handle, cqe);
            } else if (controller->m_recv_multishot_armed) {
                process_recv_completion(controller, nullptr, cqe);
            } else if (controller->m_accept_multishot_armed) {
                process_accept_completion(controller, nullptr, cqe);
            }
        }
        return;
    }

    switch (base->m_sqe_type) {
    case ACCEPT: {
        auto* awaitable = static_cast<AcceptAwaitable*>(base);
        process_accept_completion(controller, awaitable, cqe);
        break;
    }
    case CONNECT: {
        auto* awaitable = static_cast<ConnectAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_connect(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kConnectFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case RECV: {
        auto* awaitable = static_cast<RecvAwaitable*>(base);
        process_recv_completion(controller, awaitable, cqe);
        break;
    }
    case SEND: {
        auto* awaitable = static_cast<SendAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_send(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case READV: {
        auto* awaitable = static_cast<ReadvAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_readv(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kRecvFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case WRITEV: {
        auto* awaitable = static_cast<WritevAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_writev(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case FILEREAD: {
        auto* awaitable = static_cast<FileReadAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_file_read(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kReadFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case FILEWRITE: {
        auto* awaitable = static_cast<FileWriteAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_file_write(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kWriteFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case RECVFROM: {
        auto* awaitable = static_cast<RecvFromAwaitable*>(base);
        if (controller->m_recvfrom_multishot_armed) {
            process_recv_from_completion(controller, awaitable, handle, cqe);
        } else {
            if (awaitable->handle_complete(cqe, controller->m_handle)) {
                complete_and_wake(awaitable);
            } else {
                const int ret = add_recv_from(controller);
                if (ret < 0) {
                    awaitable->m_result =
                        std::unexpected(IOError(kRecvFailed, negative_ret_or_errno(ret)));
                    complete_and_wake(awaitable);
                }
            }
        }
        break;
    }
    case SENDTO: {
        auto* awaitable = static_cast<SendToAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_send_to(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case FILEWATCH: {
        auto* awaitable = static_cast<FileWatchAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_file_watch(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kReadFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case SENDFILE: {
        auto* awaitable = static_cast<SendFileAwaitable*>(base);
        if (awaitable->handle_complete(cqe, controller->m_handle)) {
            complete_and_wake(awaitable);
        } else {
            const int ret = add_send_file(controller);
            if (ret < 0) {
                awaitable->m_result =
                    std::unexpected(IOError(kSendFailed, negative_ret_or_errno(ret)));
                complete_and_wake(awaitable);
            }
        }
        break;
    }
    case SEQUENCE: {
        auto* sequence = static_cast<SequenceAwaitableBase*>(base);
        const auto event_type = sequence->active_event_type();
        controller->m_awaitable[slot] = nullptr;
        controller->advance_sqe_generation(slot);

        SequenceProgress progress = SequenceProgress::kNeedWait;
        if (slot == IOController::READ && event_type == READV) {
            io_uring_cqe ready_cqe = *cqe;
            bool deliver = cqe->res < 0;
            if (cqe->res >= 0) {
                auto* task = sequence->front();
                auto* readv_ctx = task != nullptr
                    ? static_cast<ReadvIOContext*>(task->context)
                    : nullptr;
                int immediate_res = 0;
                if (try_immediate_readv(controller->m_handle.fd, readv_ctx, immediate_res)) {
                    ready_cqe.res = immediate_res;
                    deliver = true;
                }
            }
            if (deliver) {
                progress = sequence->on_active_event(&ready_cqe, controller->m_handle);
            }
        } else {
            progress = sequence->on_active_event(cqe, controller->m_handle);
        }
        if (progress == SequenceProgress::kCompleted) {
            sequence->on_completed();
            sequence->m_waker.wake_up();
        } else {
            const int ret = add_sequence(controller);
            if (ret < 0) {
                detail::store_backend_error(
                    m_last_error_code, kNotReady, negative_ret_or_errno(ret));
                sequence->on_completed();
                sequence->m_waker.wake_up();
            }
        }
        break;
    }
    default:
        break;
    }
}

void IOUringReactor::process_accept_completion(IOController* controller,
                                             AcceptAwaitable* awaitable,
                                             struct io_uring_cqe* cqe) {
    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    auto result = io::handle_accept(cqe);
    bool completed = false; // 本次 try_complete 的返回值，不是第二个持久完成 gate。
    if (result) {
        if (awaitable) { completed = awaitable->select_ready(*result); }
        else { controller->enqueue_accepted_handle(*result); }
    } else if (!IOError::contains(result.error().code(), kNotReady)) {
        if (awaitable) {
            completed = awaitable->select_error(CompletionReason::kBackendError, result.error());
        } else {
            detail::store_backend_error(m_last_error_code,
                io_error_code_from_error(result.error()), system_code_from_error(result.error()));
        }
    }

    if (!more) {
        controller->m_accept_multishot_handle = nullptr;
        controller->m_accept_multishot_armed = false;
        if (controller->m_handle != GHandle::invalid() && !m_accept_stopping) {
            const int ret = submit_multishot_accept(controller);
            if (ret < 0) {
                if (awaitable && awaitable->select_error(CompletionReason::kBackendError,
                        IOError(kAcceptFailed, negative_ret_or_errno(ret)))) {
                    completed = true;
                } else {
                    detail::store_backend_error(m_last_error_code, kAcceptFailed, negative_ret_or_errno(ret));
                }
            }
        }
    }
    // 先完成资源重挂/错误记账，再断开 frame 入口。原请求由 terminal guard
    // 独立 recycle；MORE 完成单次 accept 不等待、更不回收持久请求。
    if (completed) {
        auto resume = detach_accept(*awaitable);
        if (!resume) {
            detail::store_backend_error(m_last_error_code, kNotReady, EINVAL);
            return;
        }
        std::move(*resume).resume();
    }
}

void IOUringReactor::process_recv_completion(IOController* controller,
                                           RecvAwaitable* awaitable,
                                           struct io_uring_cqe* cqe) {
    if (controller == nullptr) {
        return;
    }

    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    const bool cancelled = (-cqe->res == ECANCELED);
    const bool buffer_exhausted = (-cqe->res == ENOBUFS);
    const bool transient =
        (-cqe->res == EAGAIN || -cqe->res == EWOULDBLOCK || -cqe->res == EINTR);

    if (cqe->res > 0) {
        if ((cqe->flags & IORING_CQE_F_BUFFER) == 0) {
            ReadyRecvChunk chunk;
            chunk.kind = ReadyRecvChunk::Kind::Error;
            chunk.result = std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(EINVAL)));
            controller->enqueue_ready_recv(std::move(chunk));
        } else {
            const uint16_t bid = cqe_buffer_id(cqe);
            auto* pool = recv_buffer_pool(m_recv_buffer_pool);
            ReadyRecvChunk chunk;
            chunk.owner = m_recv_buffer_pool;
            chunk.data = pool != nullptr ? pool->data(bid) : nullptr;
            chunk.bid = bid;
            chunk.length = static_cast<size_t>(cqe->res);
            chunk.kind = ReadyRecvChunk::Kind::Buffer;
            chunk.recycle = recycle_recv_buffer;
            if (chunk.data == nullptr) {
                chunk.release();
                chunk.kind = ReadyRecvChunk::Kind::Error;
                chunk.result = std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(EINVAL)));
            }
            controller->enqueue_ready_recv(std::move(chunk));
        }
    } else if (cqe->res == 0) {
        ReadyRecvChunk chunk;
        chunk.kind = ReadyRecvChunk::Kind::Eof;
        chunk.result = static_cast<size_t>(0);
        controller->enqueue_ready_recv(std::move(chunk));
    } else if (!transient && !buffer_exhausted && !cancelled) {
        ReadyRecvChunk chunk;
        chunk.kind = ReadyRecvChunk::Kind::Error;
        chunk.result = std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(-cqe->res)));
        controller->enqueue_ready_recv(std::move(chunk));
    }

    if (awaitable != nullptr && !cancelled && !controller->m_recv_result_assigned) {
        bool should_deliver = false;
        if (buffer_exhausted) {
            should_deliver = controller->try_consume_ready_recv(awaitable->m_buffer,
                                                             awaitable->m_length,
                                                             awaitable->m_result);
        } else if (awaitable->handle_complete(cqe, controller->m_handle)) {
            should_deliver = controller->try_consume_ready_recv(awaitable->m_buffer,
                                                             awaitable->m_length,
                                                             awaitable->m_result);
        }

        if (should_deliver) {
            controller->m_recv_result_assigned = true;
            complete_and_wake(awaitable);
        }
    }

    if (more) {
        return;
    }

    controller->m_recv_multishot_handle = nullptr;
    controller->m_recv_multishot_armed = false;
}

void IOUringReactor::process_recv_from_completion(IOController* controller,
                                               RecvFromAwaitable* awaitable,
                                               SqeRequestHandle* handle,
                                               struct io_uring_cqe* cqe)
{
#if !GALAY_HAS_IO_URING_RECVMSG_MULTISHOT
    (void)controller;
    (void)awaitable;
    (void)handle;
    (void)cqe;
    return;
#else
    if (controller == nullptr || handle == nullptr || cqe == nullptr) {
        return;
    }

    const bool more = (cqe->flags & IORING_CQE_F_MORE) != 0;
    const bool cancelled = cqe->res == -ECANCELED;
    const bool buffer_exhausted = cqe->res == -ENOBUFS;
    const bool transient = cqe->res == -EAGAIN || cqe->res == -EWOULDBLOCK ||
                           cqe->res == -EINTR;
    const bool unsupported = cqe->res == -EOPNOTSUPP ||
                             (cqe->res == -EINVAL && !m_recvmsg_multishot_confirmed);

    if (cqe->res >= 0) {
        m_recvmsg_multishot_confirmed = true;
        ReadyRecvDatagram datagram;
        if ((cqe->flags & IORING_CQE_F_BUFFER) == 0 || !m_recvfrom_buffer_pool) {
            datagram.kind = ReadyRecvDatagram::Kind::Error;
            datagram.result = std::unexpected(
                IOError(kRecvFailed, static_cast<uint32_t>(EINVAL)));
        } else {
            const uint16_t bid = cqe_buffer_id(cqe);
            auto* pool = recv_buffer_pool(m_recvfrom_buffer_pool);
            datagram.owner = m_recvfrom_buffer_pool;
            datagram.recycle = recycle_recv_buffer;
            datagram.bid = bid;
            datagram.data = pool != nullptr ? pool->data(bid) : nullptr;

            auto* message = handle->arena != nullptr
                ? handle->arena->recv_from_message()
                : nullptr;
            auto* output = datagram.data != nullptr && message != nullptr
                ? io_uring_recvmsg_validate(datagram.data, cqe->res, message)
                : nullptr;
            if (output == nullptr || output->namelen < sizeof(sa_family_t)) {
                datagram.kind = ReadyRecvDatagram::Kind::Error;
                datagram.result = std::unexpected(
                    IOError(kRecvFailed, static_cast<uint32_t>(EINVAL)));
            } else {
                const size_t source_length = std::min(
                    {static_cast<size_t>(output->namelen),
                     static_cast<size_t>(message->msg_namelen),
                     sizeof(datagram.source)});
                std::memset(&datagram.source, 0, sizeof(datagram.source));
                std::memcpy(&datagram.source,
                            io_uring_recvmsg_name(output),
                            source_length);
                datagram.data = static_cast<char*>(io_uring_recvmsg_payload(output, message));
                datagram.length = io_uring_recvmsg_payload_length(output, cqe->res, message);
            }
        }
        controller->enqueue_ready_recv_from(std::move(datagram));
    } else if (!transient && !buffer_exhausted && !cancelled && !unsupported) {
        ReadyRecvDatagram datagram;
        datagram.kind = ReadyRecvDatagram::Kind::Error;
        datagram.result = std::unexpected(
            IOError(kRecvFailed, static_cast<uint32_t>(-cqe->res)));
        controller->enqueue_ready_recv_from(std::move(datagram));
    }

    if (awaitable != nullptr && !cancelled && !controller->m_recvfrom_result_assigned &&
        controller->try_consume_ready_recv_from(awaitable->m_buffer,
                                            awaitable->m_length,
                                            awaitable->m_from,
                                            awaitable->m_result)) {
        controller->m_recvfrom_result_assigned = true;
        complete_and_wake(awaitable);
    }

    if (more) {
        return;
    }

    controller->m_recvfrom_multishot_handle = nullptr;
    controller->m_recvfrom_multishot_armed = false;
    if (unsupported) {
        m_recvmsg_multishot_supported = false;
        m_recvmsg_multishot_confirmed = false;
    }
    if (controller->m_handle == GHandle::invalid() || awaitable == nullptr ||
        controller->m_recvfrom_result_assigned || cancelled) {
        return;
    }

    int ret = 0;
    if (m_recvmsg_multishot_supported) {
        ret = submit_multishot_recv_from(controller);
    } else {
        ret = add_recv_from_one_shot(controller, awaitable);
    }
    if (ret < 0) {
        awaitable->m_result =
            std::unexpected(IOError(kRecvFailed, negative_ret_or_errno(ret)));
        controller->m_recvfrom_result_assigned = true;
        complete_and_wake(awaitable);
    }
#endif
}

}  // namespace galay::kernel

#endif  // USE_IOURING
