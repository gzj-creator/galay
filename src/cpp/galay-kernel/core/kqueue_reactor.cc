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

#include "awaitable.h"

#include <cerrno>
#include <expected>

namespace galay::kernel {

namespace {

inline bool sequenceMaskUsesSlot(uint8_t mask, IOController::Index slot) {
    return (mask & detail::sequenceSlotMask(slot)) != 0;
}

inline uint8_t simpleEventMask(IOEventType type) {
    const uint32_t value = static_cast<uint32_t>(type);
    uint8_t mask = 0;
    if ((value & (ACCEPT | RECV | READV | RECVFROM | FILEREAD)) != 0) {
        mask = static_cast<uint8_t>(mask | detail::sequenceSlotMask(IOController::READ));
    }
    if ((value & (CONNECT | SEND | WRITEV | SENDTO | FILEWRITE | SENDFILE)) != 0) {
        mask = static_cast<uint8_t>(mask | detail::sequenceSlotMask(IOController::WRITE));
    }
    return mask;
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
    , m_last_error_code(last_error_code) {
    m_pending_changes.reserve(BATCH_THRESHOLD);
}

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
        const uint32_t registration_errno = static_cast<uint32_t>(errno);
        const int close_result = galay_close(m_kqueue_fd);
        if (close_result != 0) {
            detail::storeBackendError(
                m_last_error_code, kDisconnectError, static_cast<uint32_t>(errno));
        } else {
            detail::storeBackendError(m_last_error_code, kOpenFailed, registration_errno);
        }
        m_kqueue_fd = -1;
        return std::unexpected(IOError(kOpenFailed, registration_errno));
    }

    m_events.resize(m_max_events);
    return {};
}

KqueueReactor::~KqueueReactor() {
    if (m_kqueue_fd != -1) {
        const int close_result = galay_close(m_kqueue_fd);
        if (close_result != 0) {
            detail::storeBackendError(
                m_last_error_code, kDisconnectError, static_cast<uint32_t>(errno));
        }
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

int KqueueReactor::updateSimpleInterest(IOController* controller,
                                        IOController::Index slot,
                                        bool desired) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return -1;
    }

    RegistrationEntry* entry = nullptr;
    if (desired) {
        // controller 移动后 fd 不变，重复 arm 也必须把稳定入口重绑到新对象。
        entry = registrationEntryForController(controller);
        if (entry == nullptr) {
            return -1;
        }
    }

    const uint8_t slot_mask = detail::sequenceSlotMask(slot);
    const uint8_t old_simple = controller->m_simple_armed_mask;
    const uint8_t new_simple = desired
        ? static_cast<uint8_t>(old_simple | slot_mask)
        : static_cast<uint8_t>(old_simple & ~slot_mask);
    if (new_simple == old_simple) {
        return 0;
    }

    const uint8_t old_combined =
        static_cast<uint8_t>(old_simple | controller->m_sequence_armed_mask);
    const uint8_t new_combined =
        static_cast<uint8_t>(new_simple | controller->m_sequence_armed_mask);
    if ((old_combined & slot_mask) != (new_combined & slot_mask)) {
        const int16_t filter = slot == IOController::READ ? EVFILT_READ : EVFILT_WRITE;
        const uint16_t flags = desired ? static_cast<uint16_t>(EV_ADD | EV_CLEAR)
                                       : static_cast<uint16_t>(EV_DELETE);
        struct kevent change;
        EV_SET(&change,
               controller->m_handle.fd,
               filter,
               flags,
               0,
               0,
               desired ? entry : nullptr);
        m_pending_changes.push_back(change);
    }
    controller->m_simple_armed_mask = new_simple;

    if (m_pending_changes.size() >= BATCH_THRESHOLD) {
        return flushPendingChanges();
    }
    return 0;
}

void KqueueReactor::deleteOneShotRegistration(int fd, int16_t filter) {
    struct kevent delete_event;
    EV_SET(&delete_event, fd, filter, EV_DELETE, 0, 0, nullptr);
    const int delete_result = kevent(
        m_kqueue_fd, &delete_event, 1, nullptr, 0, nullptr);
    if (delete_result < 0 && errno != ENOENT) {
        detail::storeBackendError(
            m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    }
}

void KqueueReactor::discardPendingChanges(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return;
    }

    const uintptr_t fd = static_cast<uintptr_t>(controller->m_handle.fd);
    size_t index = 0;
    while (index < m_pending_changes.size()) {
        if (m_pending_changes[index].ident == fd) {
            const auto next = m_pending_changes.erase(m_pending_changes.begin() + index);
            index = static_cast<size_t>(next - m_pending_changes.begin());
            continue;
        }
        ++index;
    }
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
    return updateSimpleInterest(controller, IOController::READ, true);
}

int KqueueReactor::addSend(IOController* controller) {
    auto* awaitable = controller->getAwaitable<SendAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    return updateSimpleInterest(controller, IOController::WRITE, true);
}

int KqueueReactor::addReadv(IOController* controller) {
    auto* awaitable = controller->getAwaitable<ReadvAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    return updateSimpleInterest(controller, IOController::READ, true);
}

int KqueueReactor::addWritev(IOController* controller) {
    auto* awaitable = controller->getAwaitable<WritevAwaitable>();
    if (awaitable == nullptr) return -1;
    if (awaitable->handleComplete(controller->m_handle)) {
        return 1;
    }
    return updateSimpleInterest(controller, IOController::WRITE, true);
}

int KqueueReactor::addClose(IOController* controller) {
    if (controller == nullptr || controller->m_handle == GHandle::invalid()) {
        return 0;
    }

    const int fd = controller->m_handle.fd;
    discardPendingChanges(controller);
    retireRegistrationEntry(controller);
    const uint8_t armed_mask = static_cast<uint8_t>(
        controller->m_simple_armed_mask |
        controller->m_sequence_armed_mask |
        simpleEventMask(controller->m_type));
    struct kevent evs[3];
    int ev_count = 0;
    if (sequenceMaskUsesSlot(armed_mask, IOController::READ)) {
        EV_SET(&evs[ev_count++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (sequenceMaskUsesSlot(armed_mask, IOController::WRITE)) {
        EV_SET(&evs[ev_count++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    if ((static_cast<uint32_t>(controller->m_type) & FILEWATCH) != 0) {
        EV_SET(&evs[ev_count++], fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    }
    if (ev_count > 0) {
        const int delete_result = kevent(m_kqueue_fd, evs, ev_count, nullptr, 0, nullptr);
        if (delete_result < 0 && errno != ENOENT) {
            detail::storeBackendError(
                m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
        }
    }

    controller->m_type = IOEventType::INVALID;
    controller->m_awaitable[IOController::READ] = nullptr;
    controller->m_awaitable[IOController::WRITE] = nullptr;
    controller->m_sequence_owner[IOController::READ] = nullptr;
    controller->m_sequence_owner[IOController::WRITE] = nullptr;
    controller->m_simple_armed_mask = 0;
    detail::clearSequenceInterestMask(controller);

    const int close_result = galay_close(fd);
    const uint32_t close_errno = close_result == 0 ? 0 : static_cast<uint32_t>(errno);
    controller->m_handle = GHandle::invalid();
    if (close_result != 0) {
        detail::storeBackendError(m_last_error_code, kDisconnectError, close_errno);
        return -static_cast<int>(close_errno);
    }
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
    const int fd = controller->m_handle.fd;
    discardPendingChanges(controller);
    retireRegistrationEntry(controller);
    const uint8_t armed_mask = static_cast<uint8_t>(
        controller->m_simple_armed_mask |
        controller->m_sequence_armed_mask |
        simpleEventMask(controller->m_type));
    struct kevent evs[3];
    int ev_count = 0;
    if (sequenceMaskUsesSlot(armed_mask, IOController::READ)) {
        EV_SET(&evs[ev_count++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (sequenceMaskUsesSlot(armed_mask, IOController::WRITE)) {
        EV_SET(&evs[ev_count++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    if ((static_cast<uint32_t>(controller->m_type) & FILEWATCH) != 0) {
        EV_SET(&evs[ev_count++], fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
    }
    controller->m_simple_armed_mask = 0;
    controller->m_sequence_armed_mask = 0;
    if (ev_count == 0) {
        return 0;
    }
    const int result = kevent(m_kqueue_fd, evs, ev_count, nullptr, 0, nullptr);
    if (result == 0 || errno == ENOENT) {
        return 0;
    }
    detail::storeBackendError(
        m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
    return -static_cast<int>(errno);
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

        controller->removeAwaitable(event_type);
        const auto slot = filter == EVFILT_READ ? IOController::READ : IOController::WRITE;
        const bool uses_cached_interest =
            event_type == RECV || event_type == READV ||
            event_type == SEND || event_type == WRITEV;
        const bool keep_armed = event_type == RECV || event_type == READV;
        if (uses_cached_interest && !keep_armed) {
            const int interest_result = updateSimpleInterest(controller, slot, false);
            if (interest_result < 0) {
                detail::storeBackendError(
                    m_last_error_code, kNotReady, static_cast<uint32_t>(errno));
            }
        } else if (!uses_cached_interest) {
            deleteOneShotRegistration(controller->m_handle.fd, filter);
        }

        const uint8_t armed_mask = static_cast<uint8_t>(
            controller->m_simple_armed_mask | controller->m_sequence_armed_mask);
        if (controller->m_type == IOEventType::INVALID && armed_mask == 0) {
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
                controller->removeAwaitable(FILEWATCH);
                deleteOneShotRegistration(controller->m_handle.fd, EVFILT_VNODE);
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
            const uint8_t remaining_interest = detail::syncSequenceInterestMask(controller);
            if (remaining_interest == 0 &&
                controller->m_type == IOEventType::INVALID &&
                controller->m_simple_armed_mask == 0) {
                retireRegistrationEntry(controller);
            }
            owner->m_waker.wakeUp();
            return;
        }

        const int ret = addSequence(controller);
        if (ret == 1) {
            owner->onCompleted();
            const uint8_t remaining_interest = detail::syncSequenceInterestMask(controller);
            if (remaining_interest == 0 &&
                controller->m_type == IOEventType::INVALID &&
                controller->m_simple_armed_mask == 0) {
                retireRegistrationEntry(controller);
            }
            owner->m_waker.wakeUp();
        } else if (ret < 0) {
            const uint32_t sys = (ret != -1)
                ? static_cast<uint32_t>(-ret)
                : static_cast<uint32_t>(errno);
            detail::storeBackendError(m_last_error_code, kNotReady, sys);
            owner->onCompleted();
            const uint8_t remaining_interest = detail::syncSequenceInterestMask(controller);
            if (remaining_interest == 0 &&
                controller->m_type == IOEventType::INVALID &&
                controller->m_simple_armed_mask == 0) {
                retireRegistrationEntry(controller);
            }
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
    const uint8_t old_combined =
        static_cast<uint8_t>(controller->m_simple_armed_mask | armed_mask);
    const uint8_t new_combined =
        static_cast<uint8_t>(controller->m_simple_armed_mask | desired_mask);
    const uint8_t to_delete = static_cast<uint8_t>(old_combined & ~new_combined);
    const uint8_t to_add = static_cast<uint8_t>(new_combined & ~old_combined);

    if ((to_delete | to_add) == 0) {
        if (new_combined != 0 && registrationEntryForController(controller) == nullptr) {
            return -1;
        }
        controller->m_sequence_armed_mask = desired_mask;
        return 0;
    }

    struct kevent evs[4];
    int ev_count = 0;
    const int fd = controller->m_handle.fd;
    RegistrationEntry* entry = nullptr;
    if (to_add != 0) {
        entry = registrationEntryForController(controller);
        if (entry == nullptr) {
            return -1;
        }
    }

    const auto append_change = [&](IOController::Index slot, uint16_t flags) {
        const int16_t filter = slot == IOController::READ ? EVFILT_READ : EVFILT_WRITE;
        const uintptr_t fflags =
            (slot == IOController::WRITE && (flags & EV_ADD) != 0) ? NOTE_LOWAT : 0;
        const intptr_t data =
            (slot == IOController::WRITE && (flags & EV_ADD) != 0) ? 1 : 0;
        EV_SET(&evs[ev_count++],
               fd,
               filter,
               flags,
               fflags,
               data,
               (flags & EV_ADD) != 0 ? entry : nullptr);
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
