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
#include "../async/aio_file.h"

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

uint32_t ioTypeToEpollEvents(IOEventType type) {
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

uint32_t sequenceInterestToEpollEvents(detail::SequenceInterestMask mask) {
    uint32_t events = EPOLLET;
    if ((mask & detail::sequenceSlotMask(IOController::READ)) != 0) {
        events |= EPOLLIN;
    }
    if ((mask & detail::sequenceSlotMask(IOController::WRITE)) != 0) {
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
    if (m_epoll_fd != -1 && m_event_fd != -1) {
        return {};
    }

    m_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll_fd == -1) {
        detail::storeBackendError(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_event_fd == -1) {
        close(m_epoll_fd);
        m_epoll_fd = -1;
        detail::storeBackendError(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
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
        detail::storeBackendError(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    m_events.resize(m_max_events);
    return {};
}

EpollReactor::~EpollReactor() {
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
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

GHandle EpollReactor::getHandle() const {
    return {m_event_fd};
}

GHandle EpollReactor::getPollHandle() const {
    return {m_epoll_fd};
}

EpollReactor::RegistrationEntry* EpollReactor::registrationEntryForController(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return nullptr;
    }

    const int fd = controller->m_handle.fd;
    auto it = m_registration_entries.find(fd);
    if (it == m_registration_entries.end()) {
        auto entry = std::make_unique<RegistrationEntry>();
        entry->controller = controller;
        auto* raw = entry.get();
        m_registration_entries.emplace(fd, std::move(entry));
        return raw;
    }

    it->second->controller = controller;
    return it->second.get();
}

void EpollReactor::retireRegistrationEntry(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return;
    }

    const int fd = controller->m_handle.fd;
    auto it = m_registration_entries.find(fd);
    if (it == m_registration_entries.end()) {
        return;
    }

    it->second->controller = nullptr;
    m_retired_entries.push_back(std::move(it->second));
    m_registration_entries.erase(it);
}

size_t EpollReactor::findPendingChangeIndex(IOController* controller) const {
    for (size_t index = 0; index < m_pending_changes.size(); ++index) {
        if (m_pending_changes[index].controller == controller) {
            return index;
        }
    }
    return m_pending_changes.size();
}

void EpollReactor::erasePendingChange(size_t index) {
    if (index < m_pending_changes.size()) {
        m_pending_changes.erase(m_pending_changes.begin() + index);
    }
}

void EpollReactor::discardPendingChange(IOController* controller) {
    const size_t index = findPendingChangeIndex(controller);
    if (index != m_pending_changes.size()) {
        erasePendingChange(index);
    }
}

uint32_t EpollReactor::buildEvents(IOController* controller) const {
    if (controller == nullptr) {
        return EPOLLET;
    }

    uint32_t events = ioTypeToEpollEvents(controller->m_type);
    events |= controller->m_persistent_events;
    const uint32_t t = static_cast<uint32_t>(controller->m_type);
    if ((t & SEQUENCE) == 0) {
        return events;
    }

    events |= sequenceInterestToEpollEvents(controller->m_sequence_interest_mask);
    return events;
}

int EpollReactor::armPersistentRead(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    // 2026-07-14 WS 固定口径：持久 EPOLLIN 令 epoll_ctl 7,022 -> 36，吞吐提升 3.86%。
    // 正确性依赖 addRecv/addReadv 在注册前先做非阻塞乐观读取，即使旧边沿被消费也能直接取走残留数据。
    // 持久注册期间 controller 仍可能移动；每次挂起前重绑稳定入口，避免晚到事件指向 moved-from 对象。
    if (registrationEntryForController(controller) == nullptr) {
        return -1;
    }
    controller->m_persistent_events |= EPOLLIN;
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::applyEvents(IOController* controller, uint32_t events) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    const size_t index = findPendingChangeIndex(controller);
    // 未注册状态下的删除是 no-op，不能留下可能跨过 socket 析构的裸 controller 指针。
    if (events == EPOLLET && controller->m_registered_events == 0) {
        if (index != m_pending_changes.size()) {
            erasePendingChange(index);
        }
        return 0;
    }

    if (index != m_pending_changes.size()) {
        if (events == controller->m_registered_events) {
            erasePendingChange(index);
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
        m_pending_changes.push_back(PendingChange{
            .controller = controller,
            .events = events,
        });
    }

    if (m_pending_changes.size() >= BATCH_THRESHOLD) {
        return flushPendingChanges();
    }
    return 0;
}

int EpollReactor::flushPendingChanges() {
    size_t index = 0;
    while (index < m_pending_changes.size()) {
        PendingChange change = m_pending_changes[index];
        auto* controller = change.controller;
        if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
            erasePendingChange(index);
            continue;
        }

        const uint32_t events = change.events;
        if (events == controller->m_registered_events) {
            erasePendingChange(index);
            continue;
        }

        const int fd = controller->m_handle.fd;
        if (events == EPOLLET) {
            if (controller->m_registered_events == 0) {
                erasePendingChange(index);
                continue;
            }

            int ret = -1;
            do {
                ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            } while (ret == -1 && errno == EINTR);

            if (ret == 0 || errno == ENOENT) {
                controller->m_registered_events = 0;
                retireRegistrationEntry(controller);
                erasePendingChange(index);
                continue;
            }
            detail::storeBackendError(
                m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
            return -1;
        }

        struct epoll_event ev;
        ev.events = events;
        ev.data.ptr = registrationEntryForController(controller);
        if (ev.data.ptr == nullptr) {
            erasePendingChange(index);
            continue;
        }

        int ret = -1;
        if (controller->m_registered_events == 0) {
            do {
                ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
            } while (ret == -1 && errno == EINTR);
        } else {
            do {
                ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
            } while (ret == -1 && errno == EINTR);
            if (ret == -1 && errno == ENOENT) {
                do {
                    ret = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
                } while (ret == -1 && errno == EINTR);
            }
        }

        if (ret == 0) {
            controller->m_registered_events = events;
            erasePendingChange(index);
            continue;
        }

        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
        return -1;
    }
    return 0;
}

int EpollReactor::addAccept(IOController* controller) {
    auto* awaitable = controller->getAwaitable<AcceptAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addConnect(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ConnectAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addRecv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<RecvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return armPersistentRead(controller);
}

int EpollReactor::addSend(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addReadv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ReadvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return armPersistentRead(controller);
}

int EpollReactor::addWritev(IOController* controller) {
    auto* awaitable = controller->getAwaitable<WritevAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addSendFile(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendFileAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addClose(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    const int fd = controller->m_handle.fd;
    discardPendingChange(controller);

    controller->m_type = IOEventType::INVALID;
    controller->m_awaitable[IOController::READ] = nullptr;
    controller->m_awaitable[IOController::WRITE] = nullptr;
    controller->m_sequence_owner[IOController::READ] = nullptr;
    controller->m_sequence_owner[IOController::WRITE] = nullptr;
    controller->m_persistent_events = 0;
    detail::clearSequenceInterestMask(controller);
    controller->m_registered_events = 0;
    retireRegistrationEntry(controller);

    close(fd);
    controller->m_handle = GHandle::invalid();
    return 0;
}

int EpollReactor::addFileRead(IOController* controller) {
    return applyEvents(controller, EPOLLIN | EPOLLET);
}

int EpollReactor::addFileWrite(IOController* controller) {
    return applyEvents(controller, EPOLLOUT | EPOLLET);
}

int EpollReactor::addRecvFrom(IOController* controller) {
    auto* awaitable = controller->getAwaitable<RecvFromAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addSendTo(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendToAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return kImmediateReady;
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addFileWatch(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileWatchAwaitable>();
    if (awaitable == nullptr) return -1;
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::addSequence(IOController* controller) {
    if (controller == nullptr) {
        return -1;
    }
    const auto desired_mask = detail::syncSequenceInterestMask(controller);
    if ((desired_mask & detail::sequenceSlotMask(IOController::READ)) != 0) {
        return armPersistentRead(controller);
    }
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::remove(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }
    controller->m_persistent_events = 0;
    return applyEvents(controller, EPOLLET);
}

int EpollReactor::processSequence(IOEventType type, IOController* controller) {
    const uint32_t events = sequenceInterestToEpollEvents(detail::sequenceInterestMask(type));
    if (events == EPOLLET) {
        return -1;
    }
    return applyEvents(controller, events);
}

void EpollReactor::syncEvents(IOController* controller) {
    if (controller != nullptr && (static_cast<uint32_t>(controller->m_type) & SEQUENCE) != 0) {
        (void)detail::syncSequenceInterestMask(controller);
    }
    const uint32_t events = buildEvents(controller);
    if (applyEvents(controller, events) < 0) {
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

void EpollReactor::poll(int timeout_ms, WakeCoordinator& wake_coordinator) {
    if (flushPendingChanges() < 0) {
        return;
    }

    const int nev = epoll_wait(m_epoll_fd, m_events.data(), m_max_events, timeout_ms);
    if (nev < 0) {
        if (errno == EINTR) {
            return;
        }
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
        return;
    }

    for (int i = 0; i < nev; ++i) {
        struct epoll_event& ev = m_events[i];
        if (ev.data.ptr == nullptr) {
            uint64_t val = 0;
            while (read(m_event_fd, &val, sizeof(val)) > 0) {}
            wake_coordinator.cancelPendingWake();
            continue;
        }

        if (ev.events & EPOLLERR) {
            ev.events |= (EPOLLIN | EPOLLOUT);
        }

        processEvent(ev);
    }
}

void EpollReactor::processEvent(struct epoll_event& ev) {
    auto* entry = static_cast<RegistrationEntry*>(ev.data.ptr);
    auto* controller = entry ? entry->controller : nullptr;
    if (!controller ||
        controller->m_type == IOEventType::INVALID ||
        controller->m_handle == GHandle::invalid()) {
        return;
    }

    const uint32_t t = static_cast<uint32_t>(controller->m_type);
    const auto complete_one_shot = [this, controller](auto* awaitable,
                                                      IOEventType event_type) {
        if (awaitable == nullptr || !awaitable->handleComplete(controller->m_handle)) {
            return;
        }

        Waker waker = awaitable->m_waker;
        controller->removeAwaitable(event_type);
        syncEvents(controller);
        (void)flushPendingChanges();
        waker.wakeUp();
    };

    if (ev.events & EPOLLIN) {
        if (t & ACCEPT) {
            complete_one_shot(controller->getAwaitable<AcceptAwaitable>(), ACCEPT);
        } else if (t & RECV) {
            complete_one_shot(controller->getAwaitable<RecvAwaitable>(), RECV);
        } else if (t & READV) {
            complete_one_shot(controller->getAwaitable<ReadvAwaitable>(), READV);
        } else if (t & RECVFROM) {
            complete_one_shot(controller->getAwaitable<RecvFromAwaitable>(), RECVFROM);
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
                Waker waker = aio_awaitable->m_waker;
                controller->removeAwaitable(FILEREAD);
                syncEvents(controller);
                (void)flushPendingChanges();
                waker.wakeUp();
            }
        } else if (t & FILEWATCH) {
            auto* awaitable = controller->getAwaitable<FileWatchAwaitable>();
            if (awaitable) {
                bool completed = false;
                std::expected<FileWatchResult, IOError> first_result =
                    std::unexpected(IOError(kReadFailed, 0));
                while (true) {
                    const ssize_t len =
                        read(controller->m_handle.fd, awaitable->m_buffer, awaitable->m_buffer_size);
                    if (len > 0) {
                        auto parsed = io::parseInotifyEvents(awaitable->m_buffer,
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
                Waker waker = awaitable->m_waker;
                controller->removeAwaitable(FILEWATCH);
                syncEvents(controller);
                (void)flushPendingChanges();
                waker.wakeUp();
            }
        }
    }

    const uint32_t after_read_type = static_cast<uint32_t>(controller->m_type);
    if (ev.events & EPOLLOUT) {
        if (after_read_type & CONNECT) {
            complete_one_shot(controller->getAwaitable<ConnectAwaitable>(), CONNECT);
        } else if (after_read_type & SEND) {
            complete_one_shot(controller->getAwaitable<SendAwaitable>(), SEND);
        } else if (after_read_type & WRITEV) {
            complete_one_shot(controller->getAwaitable<WritevAwaitable>(), WRITEV);
        } else if (after_read_type & SENDTO) {
            complete_one_shot(controller->getAwaitable<SendToAwaitable>(), SENDTO);
        } else if (after_read_type & FILEWRITE) {
            complete_one_shot(controller->getAwaitable<FileWriteAwaitable>(), FILEWRITE);
        } else if (after_read_type & SENDFILE) {
            complete_one_shot(controller->getAwaitable<SendFileAwaitable>(), SENDFILE);
        }
    }

    if (static_cast<uint32_t>(controller->m_type) & SEQUENCE) {
        const auto dispatch_owner = [this, controller](SequenceAwaitableBase* owner) -> bool {
            if (owner == nullptr) {
                return false;
            }

            const auto progress = owner->onActiveEvent(controller->m_handle);
            if (progress == SequenceProgress::kCompleted) {
                owner->onCompleted();
                (void)detail::syncSequenceInterestMask(controller);
                syncEvents(controller);
                (void)flushPendingChanges();
                owner->m_waker.wakeUp();
                return true;
            }

            const int ret = addSequence(controller);
            if (ret == kImmediateReady) {
                owner->onCompleted();
                (void)detail::syncSequenceInterestMask(controller);
                syncEvents(controller);
                (void)flushPendingChanges();
                owner->m_waker.wakeUp();
                return true;
            } else if (ret < 0) {
                const uint32_t sys = (ret != -1)
                    ? static_cast<uint32_t>(-ret)
                    : static_cast<uint32_t>(errno);
                detail::storeBackendError(m_last_error_code, kNotReady, sys);
                owner->onCompleted();
                (void)detail::syncSequenceInterestMask(controller);
                syncEvents(controller);
                (void)flushPendingChanges();
                owner->m_waker.wakeUp();
                return true;
            }
            return false;
        };

        SequenceAwaitableBase* dispatched = nullptr;
        if ((ev.events & EPOLLIN) != 0) {
            auto* owner = controller->m_sequence_owner[IOController::READ];
            if (owner != nullptr && owner->waitsOn(IOController::READ)) {
                if (dispatch_owner(owner)) {
                    return;
                }
                dispatched = owner;
            }
        }
        if ((ev.events & EPOLLOUT) != 0) {
            auto* owner = controller->m_sequence_owner[IOController::WRITE];
            if (owner != nullptr &&
                owner != dispatched &&
                owner->waitsOn(IOController::WRITE)) {
                if (dispatch_owner(owner)) {
                    return;
                }
            }
        }
    }

    syncEvents(controller);
}

}  // namespace galay::kernel

#endif  // USE_EPOLL
