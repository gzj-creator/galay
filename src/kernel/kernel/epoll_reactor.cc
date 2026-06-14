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

#include "kernel/kernel/awaitable.h"
#include "kernel/async/aio_file.h"

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <expected>
#include <stdexcept>
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
    : m_epoll_fd(epoll_create1(EPOLL_CLOEXEC))
    , m_event_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    , m_max_events(max_events)
    , m_last_error_code(last_error_code) {
    if (m_epoll_fd == -1) {
        throw std::runtime_error("Failed to create epoll");
    }

    if (m_event_fd == -1) {
        close(m_epoll_fd);
        throw std::runtime_error("Failed to create eventfd");
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = nullptr;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_event_fd, &ev) == -1) {
        close(m_epoll_fd);
        close(m_event_fd);
        throw std::runtime_error("Failed to add eventfd to epoll");
    }

    m_events.resize(m_max_events);
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

int EpollReactor::wakeReadFdForTest() const {
    return m_event_fd;
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
    const uint32_t t = static_cast<uint32_t>(controller->m_type);
    if ((t & SEQUENCE) == 0) {
        return events;
    }

    events |= sequenceInterestToEpollEvents(controller->m_sequence_interest_mask);
    return events;
}

int EpollReactor::applyEvents(IOController* controller, uint32_t events) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    const size_t index = findPendingChangeIndex(controller);
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
                erasePendingChange(index);
                continue;
            }
            detail::storeBackendError(
                m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
            return -1;
        }

        struct epoll_event ev;
        ev.events = events;
        ev.data.ptr = controller;

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
    return applyEvents(controller, buildEvents(controller));
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
    return applyEvents(controller, buildEvents(controller));
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
    detail::clearSequenceInterestMask(controller);
    controller->m_registered_events = 0;

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
    (void)detail::syncSequenceInterestMask(controller);
    return applyEvents(controller, buildEvents(controller));
}

int EpollReactor::remove(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }
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
    auto* controller = static_cast<IOController*>(ev.data.ptr);
    if (!controller || controller->m_type == IOEventType::INVALID) {
        return;
    }

    const uint32_t t = static_cast<uint32_t>(controller->m_type);

    if (ev.events & EPOLLIN) {
        if (t & ACCEPT) {
            completeAwaitableAndWake(controller, controller->getAwaitable<AcceptAwaitable>());
        } else if (t & RECV) {
            completeAwaitableAndWake(controller, controller->getAwaitable<RecvAwaitable>());
        } else if (t & READV) {
            completeAwaitableAndWake(controller, controller->getAwaitable<ReadvAwaitable>());
        } else if (t & RECVFROM) {
            completeAwaitableAndWake(controller, controller->getAwaitable<RecvFromAwaitable>());
        } else if (t & FILEREAD) {
            auto* aio_awaitable =
                static_cast<galay::async::AioCommitAwaitable*>(controller->m_awaitable[IOController::READ]);
            if (aio_awaitable) {
                uint64_t completed = 0;
                const ssize_t n = read(controller->m_handle.fd, &completed, sizeof(completed));
                if (n == static_cast<ssize_t>(sizeof(completed)) && completed > 0) {
                    const size_t expected_events = aio_awaitable->m_pending_count;
                    std::vector<ssize_t> results;
                    results.reserve(expected_events);

                    while (results.size() < expected_events) {
                        std::vector<struct io_event> events(expected_events - results.size());
                        const int num_events = io_getevents(aio_awaitable->m_aio_ctx,
                                                            1,
                                                            static_cast<long>(events.size()),
                                                            events.data(),
                                                            nullptr);
                        if (num_events <= 0) {
                            aio_awaitable->m_result = std::unexpected(IOError(kReadFailed, errno));
                            break;
                        }
                        for (int i = 0; i < num_events; ++i) {
                            results.push_back(events[static_cast<size_t>(i)].res);
                        }
                    }

                    if (results.size() == expected_events) {
                        aio_awaitable->m_result = std::move(results);
                    } else {
                        aio_awaitable->m_result = std::unexpected(IOError(kReadFailed, errno));
                    }
                } else {
                    aio_awaitable->m_result = std::unexpected(IOError(kReadFailed, errno));
                }

                if (applyEvents(controller, EPOLLET) < 0 && errno != ENOENT) {
                    detail::storeBackendError(
                        m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
                }
                aio_awaitable->m_waker.wakeUp();
            }
        } else if (t & FILEWATCH) {
            auto* awaitable = controller->getAwaitable<FileWatchAwaitable>();
            if (awaitable) {
                const ssize_t len =
                    read(controller->m_handle.fd, awaitable->m_buffer, awaitable->m_buffer_size);
                if (len > 0) {
                    auto* event = reinterpret_cast<struct inotify_event*>(awaitable->m_buffer);
                    FileWatchResult result;
                    result.isDir = (event->mask & IN_ISDIR) != 0;
                    if (event->len > 0) {
                        result.name = std::string(event->name);
                    }

                    uint32_t mask = 0;
                    if (event->mask & IN_ACCESS) mask |= static_cast<uint32_t>(FileWatchEvent::Access);
                    if (event->mask & IN_MODIFY) mask |= static_cast<uint32_t>(FileWatchEvent::Modify);
                    if (event->mask & IN_ATTRIB) mask |= static_cast<uint32_t>(FileWatchEvent::Attrib);
                    if (event->mask & IN_CLOSE_WRITE) mask |= static_cast<uint32_t>(FileWatchEvent::CloseWrite);
                    if (event->mask & IN_CLOSE_NOWRITE) mask |= static_cast<uint32_t>(FileWatchEvent::CloseNoWrite);
                    if (event->mask & IN_OPEN) mask |= static_cast<uint32_t>(FileWatchEvent::Open);
                    if (event->mask & IN_MOVED_FROM) mask |= static_cast<uint32_t>(FileWatchEvent::MovedFrom);
                    if (event->mask & IN_MOVED_TO) mask |= static_cast<uint32_t>(FileWatchEvent::MovedTo);
                    if (event->mask & IN_CREATE) mask |= static_cast<uint32_t>(FileWatchEvent::Create);
                    if (event->mask & IN_DELETE) mask |= static_cast<uint32_t>(FileWatchEvent::Delete);
                    if (event->mask & IN_DELETE_SELF) mask |= static_cast<uint32_t>(FileWatchEvent::DeleteSelf);
                    if (event->mask & IN_MOVE_SELF) mask |= static_cast<uint32_t>(FileWatchEvent::MoveSelf);
                    result.event = static_cast<FileWatchEvent>(mask);
                    awaitable->m_result = std::move(result);
                } else if (len == 0) {
                    awaitable->m_result = std::unexpected(IOError(kReadFailed, 0));
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    awaitable->m_result =
                        std::unexpected(IOError(kReadFailed, static_cast<uint32_t>(errno)));
                }

                awaitable->handleComplete(controller->m_handle);
                awaitable->m_waker.wakeUp();
            }
        }
    }

    if (ev.events & EPOLLOUT) {
        if (t & CONNECT) {
            completeAwaitableAndWake(controller, controller->getAwaitable<ConnectAwaitable>());
        } else if (t & SEND) {
            completeAwaitableAndWake(controller, controller->getAwaitable<SendAwaitable>());
        } else if (t & WRITEV) {
            completeAwaitableAndWake(controller, controller->getAwaitable<WritevAwaitable>());
        } else if (t & SENDTO) {
            completeAwaitableAndWake(controller, controller->getAwaitable<SendToAwaitable>());
        } else if (t & FILEWRITE) {
            completeAwaitableAndWake(controller, controller->getAwaitable<FileWriteAwaitable>());
        } else if (t & SENDFILE) {
            completeAwaitableAndWake(controller, controller->getAwaitable<SendFileAwaitable>());
        }
    }

    if (t & SEQUENCE) {
        const auto dispatch_owner = [this, controller](SequenceAwaitableBase* owner) {
            if (owner == nullptr) {
                return;
            }

            const auto progress = owner->onActiveEvent(controller->m_handle);
            if (progress == SequenceProgress::kCompleted) {
                (void)detail::syncSequenceInterestMask(controller);
                owner->m_waker.wakeUp();
                return;
            }

            const int ret = addSequence(controller);
            if (ret == kImmediateReady) {
                owner->m_waker.wakeUp();
            } else if (ret < 0) {
                const uint32_t sys = (ret != -1)
                    ? static_cast<uint32_t>(-ret)
                    : static_cast<uint32_t>(errno);
                detail::storeBackendError(m_last_error_code, kNotReady, sys);
                owner->m_waker.wakeUp();
            }
        };

        SequenceAwaitableBase* dispatched = nullptr;
        if ((ev.events & EPOLLIN) != 0) {
            auto* owner = controller->m_sequence_owner[IOController::READ];
            if (owner != nullptr && owner->waitsOn(IOController::READ)) {
                dispatch_owner(owner);
                dispatched = owner;
            }
        }
        if ((ev.events & EPOLLOUT) != 0) {
            auto* owner = controller->m_sequence_owner[IOController::WRITE];
            if (owner != nullptr &&
                owner != dispatched &&
                owner->waitsOn(IOController::WRITE)) {
                dispatch_owner(owner);
            }
        }
    }

    syncEvents(controller);
}

}  // namespace galay::kernel

#endif  // USE_EPOLL
