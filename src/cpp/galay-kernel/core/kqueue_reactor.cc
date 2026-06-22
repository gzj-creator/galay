/**
 * @file kqueue_reactor.cc
 * @brief macOS/BSD kqueue reactor 实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 BSD kqueue 实现 IO 事件注册、稳定注册入口管理、
 * sequence 感兴趣位同步和事件分发。
 */

#include "kqueue_reactor.h"

#ifdef USE_KQUEUE

#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <cerrno>
#include <expected>

namespace galay::kernel {

namespace {

inline bool sequenceMaskUsesSlot(uint8_t mask, IOController::Index slot) {
    return (mask & detail::sequenceSlotMask(slot)) != 0;
}

}  // namespace

KqueueReactor::RegistrationEntry* KqueueReactor::registrationEntryForController(IOController* controller) {
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

void KqueueReactor::retireRegistrationEntry(IOController* controller) {
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

KqueueReactor::KqueueReactor(int max_events, std::atomic<uint64_t>& last_error_code)
    : m_max_events(max_events)
    , m_last_error_code(last_error_code) {}

std::expected<void, IOError> KqueueReactor::start()
{
    if (m_kqueue_fd != -1) {
        return {};
    }

    m_kqueue_fd = kqueue();
    if (m_kqueue_fd == -1) {
        detail::storeBackendError(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    struct kevent ev;
    EV_SET(&ev, WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr) < 0) {
        galay_close(m_kqueue_fd);
        m_kqueue_fd = -1;
        detail::storeBackendError(m_last_error_code, kOpenFailed, static_cast<uint32_t>(errno));
        return std::unexpected(IOError(kOpenFailed, static_cast<uint32_t>(errno)));
    }

    m_events.resize(m_max_events);
    return {};
}

KqueueReactor::~KqueueReactor() {
    if (m_kqueue_fd != -1) {
        galay_close(m_kqueue_fd);
    }
}

void KqueueReactor::notify() {
    struct kevent ev;
    EV_SET(&ev, WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    if (kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr) < 0) {
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

GHandle KqueueReactor::getHandle() const {
    return {m_kqueue_fd};
}

int KqueueReactor::addAccept(IOController* controller) {
    auto* awaitable = controller->getAwaitable<AcceptAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addConnect(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ConnectAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addRecv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<RecvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addSend(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addReadv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ReadvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addWritev(IOController* controller) {
    auto* awaitable = controller->getAwaitable<WritevAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addClose(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    const int fd = controller->m_handle.fd;
    retireRegistrationEntry(controller);
    struct kevent evs[2];
    EV_SET(&evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)kevent(m_kqueue_fd, evs, 2, nullptr, 0, nullptr);

    controller->m_type = IOEventType::INVALID;
    controller->m_awaitable[IOController::READ] = nullptr;
    controller->m_awaitable[IOController::WRITE] = nullptr;
    controller->m_sequence_owner[IOController::READ] = nullptr;
    controller->m_sequence_owner[IOController::WRITE] = nullptr;
    detail::clearSequenceInterestMask(controller);

    galay_close(fd);
    controller->m_handle = GHandle::invalid();
    return 0;
}

int KqueueReactor::addFileRead(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileReadAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addFileWrite(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileWriteAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addRecvFrom(IOController* controller) {
    auto* awaitable = controller->getAwaitable<RecvFromAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addSendTo(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendToAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addFileWatch(IOController* controller) {
    auto* awaitable = controller->getAwaitable<FileWatchAwaitable>();
    if (awaitable == nullptr) return -1;
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;

    unsigned int fflags = 0;
    const uint32_t events = static_cast<uint32_t>(awaitable->m_events);
    if (events & static_cast<uint32_t>(FileWatchEvent::Modify)) fflags |= NOTE_WRITE;
    if (events & static_cast<uint32_t>(FileWatchEvent::DeleteSelf)) fflags |= NOTE_DELETE;
    if (events & static_cast<uint32_t>(FileWatchEvent::MoveSelf)) fflags |= NOTE_RENAME;
    if (events & static_cast<uint32_t>(FileWatchEvent::Attrib)) fflags |= NOTE_ATTRIB;
    if (events & static_cast<uint32_t>(FileWatchEvent::Modify)) fflags |= NOTE_EXTEND;

    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, fflags, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addSendFile(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendFileAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    auto* entry = registrationEntryForController(controller);
    if (entry == nullptr) return -1;
    struct kevent ev;
    EV_SET(&ev, controller->m_handle.fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, entry);
    return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);
}

int KqueueReactor::addSequence(IOController* controller) {
    if (controller == nullptr) {
        return -1;
    }
    return syncSequenceRegistration(controller);
}

int KqueueReactor::remove(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }
    retireRegistrationEntry(controller);
    struct kevent evs[2];
    EV_SET(&evs[0], controller->m_handle.fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&evs[1], controller->m_handle.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    controller->m_sequence_armed_mask = 0;
    return kevent(m_kqueue_fd, evs, 2, nullptr, 0, nullptr);
}

int KqueueReactor::flushPendingChanges() {
    if (m_pending_changes.empty()) {
        return 0;
    }
    while (true) {
        const int ret = kevent(m_kqueue_fd, m_pending_changes.data(),
                               static_cast<int>(m_pending_changes.size()),
                               nullptr, 0, nullptr);
        if (ret >= 0) {
            m_pending_changes.clear();
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        const uint32_t sys = static_cast<uint32_t>(errno);
        detail::storeBackendError(m_last_error_code, kNotReady, sys);
        return -1;
    }
}

void KqueueReactor::poll(const struct timespec& timeout, WakeCoordinator& wake_coordinator) {
    if (flushPendingChanges() < 0) {
        return;
    }
    const int nev = kevent(m_kqueue_fd, nullptr, 0, m_events.data(), m_max_events, &timeout);
    if (nev < 0) {
        if (errno == EINTR) {
            return;
        }
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
        return;
    }

    for (int i = 0; i < nev; ++i) {
        struct kevent& ev = m_events[i];
        if (ev.filter == EVFILT_USER && ev.ident == WAKE_IDENT) {
            wake_coordinator.cancelPendingWake();
            continue;
        }
        if (ev.flags & EV_ERROR) {
            if (ev.data > 0) {
                detail::storeBackendError(
                    m_last_error_code, kNotReady, static_cast<uint32_t>(ev.data));
            }
            continue;
        }
        if (!ev.udata) {
            continue;
        }
        processEvent(ev);
    }
}

void KqueueReactor::processEvent(struct kevent& ev) {
    auto* entry = static_cast<RegistrationEntry*>(ev.udata);
    auto* controller = entry ? entry->controller : nullptr;
    if (!controller || controller->m_type == IOEventType::INVALID ||
        controller->m_handle == GHandle::invalid() ||
        static_cast<uintptr_t>(controller->m_handle.fd) != ev.ident) {
        return;
    }

    const uint32_t t = static_cast<uint32_t>(controller->m_type);
    const auto complete_one_shot = [this, controller](auto* awaitable,
                                                      IOEventType event_type,
                                                      int16_t filter) {
        if (awaitable == nullptr || !awaitable->handleComplete(controller->m_handle)) {
            return;
        }

        const int fd = controller->m_handle.fd;
        controller->removeAwaitable(event_type);

        struct kevent delete_event;
        EV_SET(&delete_event, fd, filter, EV_DELETE, 0, 0, nullptr);
        (void)kevent(m_kqueue_fd, &delete_event, 1, nullptr, 0, nullptr);

        if (controller->m_type == IOEventType::INVALID) {
            retireRegistrationEntry(controller);
        }
        awaitable->m_waker.wakeUp();
    };

    if (ev.filter == EVFILT_READ) {
        if (t & ACCEPT) {
            complete_one_shot(controller->getAwaitable<AcceptAwaitable>(), ACCEPT, EVFILT_READ);
        } else if (t & RECV) {
            complete_one_shot(controller->getAwaitable<RecvAwaitable>(), RECV, EVFILT_READ);
        } else if (t & READV) {
            complete_one_shot(controller->getAwaitable<ReadvAwaitable>(), READV, EVFILT_READ);
        } else if (t & RECVFROM) {
            complete_one_shot(controller->getAwaitable<RecvFromAwaitable>(), RECVFROM, EVFILT_READ);
        } else if (t & FILEREAD) {
            complete_one_shot(controller->getAwaitable<FileReadAwaitable>(), FILEREAD, EVFILT_READ);
        }
    } else if (ev.filter == EVFILT_WRITE) {
        if (t & CONNECT) {
            complete_one_shot(controller->getAwaitable<ConnectAwaitable>(), CONNECT, EVFILT_WRITE);
        } else if (t & SEND) {
            complete_one_shot(controller->getAwaitable<SendAwaitable>(), SEND, EVFILT_WRITE);
        } else if (t & WRITEV) {
            complete_one_shot(controller->getAwaitable<WritevAwaitable>(), WRITEV, EVFILT_WRITE);
        } else if (t & SENDTO) {
            complete_one_shot(controller->getAwaitable<SendToAwaitable>(), SENDTO, EVFILT_WRITE);
        } else if (t & FILEWRITE) {
            complete_one_shot(controller->getAwaitable<FileWriteAwaitable>(), FILEWRITE, EVFILT_WRITE);
        } else if (t & SENDFILE) {
            complete_one_shot(controller->getAwaitable<SendFileAwaitable>(), SENDFILE, EVFILT_WRITE);
        }
    } else if (ev.filter == EVFILT_VNODE) {
        if (t & FILEWATCH) {
            auto* awaitable = controller->getAwaitable<FileWatchAwaitable>();
            if (awaitable) {
                FileWatchResult result;
                result.isDir = false;

                uint32_t mask = 0;
                if (ev.fflags & NOTE_WRITE) mask |= static_cast<uint32_t>(FileWatchEvent::Modify);
                if (ev.fflags & NOTE_DELETE) mask |= static_cast<uint32_t>(FileWatchEvent::DeleteSelf);
                if (ev.fflags & NOTE_RENAME) mask |= static_cast<uint32_t>(FileWatchEvent::MoveSelf);
                if (ev.fflags & NOTE_ATTRIB) mask |= static_cast<uint32_t>(FileWatchEvent::Attrib);
                if (ev.fflags & NOTE_EXTEND) mask |= static_cast<uint32_t>(FileWatchEvent::Modify);
                result.event = static_cast<FileWatchEvent>(mask);

                awaitable->m_result = std::move(result);
                const bool completed = awaitable->handleComplete(controller->m_handle);
                const int fd = controller->m_handle.fd;
                controller->removeAwaitable(FILEWATCH);
                struct kevent delete_event;
                EV_SET(&delete_event, fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
                (void)kevent(m_kqueue_fd, &delete_event, 1, nullptr, 0, nullptr);
                if (controller->m_type == IOEventType::INVALID) {
                    retireRegistrationEntry(controller);
                }
                if (!completed) {
                    return;
                }
                awaitable->m_waker.wakeUp();
            }
        }
    }

    if (t & SEQUENCE) {
        SequenceAwaitableBase* owner = nullptr;
        if (ev.filter == EVFILT_READ) {
            owner = controller->m_sequence_owner[IOController::READ];
            if (owner != nullptr && !owner->waitsOn(IOController::READ)) {
                owner = nullptr;
            }
        } else if (ev.filter == EVFILT_WRITE) {
            owner = controller->m_sequence_owner[IOController::WRITE];
            if (owner != nullptr && !owner->waitsOn(IOController::WRITE)) {
                owner = nullptr;
            }
        }

        if (owner == nullptr) {
            return;
        }

        const auto progress = owner->onActiveEvent(controller->m_handle);
        if (progress == SequenceProgress::kCompleted) {
            owner->onCompleted();
            (void)detail::syncSequenceInterestMask(controller);
            owner->m_waker.wakeUp();
            return;
        }

        const int ret = addSequence(controller);
        if (ret == 1) {
            owner->onCompleted();
            (void)detail::syncSequenceInterestMask(controller);
            owner->m_waker.wakeUp();
        } else if (ret < 0) {
            const uint32_t sys = (ret != -1)
                ? static_cast<uint32_t>(-ret)
                : static_cast<uint32_t>(errno);
            detail::storeBackendError(m_last_error_code, kNotReady, sys);
            owner->onCompleted();
            (void)detail::syncSequenceInterestMask(controller);
            owner->m_waker.wakeUp();
        }
    }
}

int KqueueReactor::syncSequenceRegistration(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }
    const auto desired_mask = detail::syncSequenceInterestMask(controller);
    return applySequenceInterest(controller, desired_mask);
}

int KqueueReactor::applySequenceInterest(IOController* controller, uint8_t desired_mask) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    const uint8_t armed_mask = controller->m_sequence_armed_mask;
    const uint8_t to_delete = static_cast<uint8_t>(armed_mask & ~desired_mask);
    const uint8_t to_add = static_cast<uint8_t>(desired_mask & ~armed_mask);

    if ((to_delete | to_add) == 0) {
        return 0;
    }

    struct kevent evs[4];
    int ev_count = 0;
    const int fd = controller->m_handle.fd;

    const auto append_change = [&](IOController::Index slot, uint16_t flags) {
        const int16_t filter = slot == IOController::READ ? EVFILT_READ : EVFILT_WRITE;
        const uintptr_t fflags =
            (slot == IOController::WRITE && (flags & EV_ADD) != 0) ? NOTE_LOWAT : 0;
        const intptr_t data =
            (slot == IOController::WRITE && (flags & EV_ADD) != 0) ? 1 : 0;
        auto* entry = registrationEntryForController(controller);
        EV_SET(&evs[ev_count++], fd, filter, flags, fflags, data, entry);
    };

    if (sequenceMaskUsesSlot(to_delete, IOController::READ)) {
        append_change(IOController::READ, EV_DELETE);
    }
    if (sequenceMaskUsesSlot(to_delete, IOController::WRITE)) {
        append_change(IOController::WRITE, EV_DELETE);
    }
    if (sequenceMaskUsesSlot(to_add, IOController::READ)) {
        append_change(IOController::READ, EV_ADD | EV_CLEAR);
    }
    if (sequenceMaskUsesSlot(to_add, IOController::WRITE)) {
        append_change(IOController::WRITE, EV_ADD | EV_CLEAR);
    }

    const int ret = kevent(m_kqueue_fd, evs, ev_count, nullptr, 0, nullptr);
    if (ret == 0) {
        controller->m_sequence_armed_mask = desired_mask;
    }
    return ret;
}

}  // namespace galay::kernel

#endif  // USE_KQUEUE
