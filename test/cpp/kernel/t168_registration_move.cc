/**
 * @file t168_registration_move.cc
 * @brief 验证 owner pointer 转移后持久 reactor 注册仍指向稳定的 IOController。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/wake_coordinator.h>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_reactor.h>
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_reactor.h>
#endif

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>

using namespace galay::kernel;

static_assert(!std::is_move_constructible_v<IOController>);
static_assert(!std::is_move_assignable_v<IOController>);

namespace {

bool closeFd(int& fd) {
    if (fd < 0) {
        return true;
    }
    const int close_result = ::close(fd);
    fd = -1;
    return close_result == 0;
}

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

#ifdef USE_EPOLL
bool pendingRegistrationMoveCanBeCancelled() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0 ||
        !setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        if (!first_closed || !second_closed) {
            std::cerr << "[T168] pending-move setup cleanup failed\n";
        }
        return false;
    }

    std::atomic<uint64_t> last_error{0};
    EpollReactor reactor(8, last_error);
    if (!reactor.start()) {
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        if (!first_closed || !second_closed) {
            std::cerr << "[T168] pending-move start cleanup failed\n";
        }
        return false;
    }

    auto source = std::make_unique<IOController>(GHandle{.fd = fds[0]});
    char recv_buffer = 0;
    RecvAwaitable recv_awaitable(source.get(), &recv_buffer, 1);
    if (!source->fillAwaitable(RECV, &recv_awaitable) ||
        reactor.addRecv(source.get()) != 0) {
        std::cerr << "[T168] failed to queue pending recv before move\n";
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        if (!first_closed || !second_closed) {
            std::cerr << "[T168] pending-move registration cleanup failed\n";
        }
        return false;
    }

    auto* const stable_controller = source.get();
    auto* const owner_slot = source->m_registration_owner_slot;
    auto moved = std::move(source);
    source.reset();
    bool passed = true;
    if (source || moved.get() != stable_controller || owner_slot == nullptr ||
        moved->m_registration_owner_slot != owner_slot || *owner_slot != stable_controller ||
        recv_awaitable.m_controller != stable_controller ||
        moved->m_awaitable[IOController::READ] != &recv_awaitable) {
        std::cerr << "[T168] pending owner transfer changed published identities\n";
        passed = false;
    }
    moved->removeAwaitable(RECV);
    const int remove_result = reactor.remove(moved.get());
    const int flush_result = reactor.flushPendingChanges();
    if (remove_result != 0 || flush_result != 0) {
        std::cerr << "[T168] failed to cancel moved pending registration\n";
        passed = false;
    } else if (moved->m_registered_events != 0 ||
               moved->m_registration_owner_slot != owner_slot || *owner_slot != stable_controller) {
        std::cerr << "[T168] pending registration survived owner transfer and remove\n";
        passed = false;
    }

    if (reactor.addClose(moved.get()) != 0) {
        std::cerr << "[T168] pending-move controller cleanup failed\n";
        passed = false;
    } else {
        fds[0] = -1;
        if (moved->m_registration_owner_slot != nullptr ||
            moved->m_handle != GHandle::invalid()) {
            std::cerr << "[T168] close retained controller registration or handle\n";
            passed = false;
        }
    }
    const bool first_closed = closeFd(fds[0]);
    const bool second_closed = closeFd(fds[1]);
    if (!first_closed || !second_closed || last_error.load() != 0) {
        std::cerr << "[T168] pending-move socket cleanup failed\n";
        passed = false;
    }
    return passed;
}
#endif

}  // namespace

int main() {
#if !defined(USE_EPOLL) && !defined(USE_KQUEUE)
    std::cout << "T168-RegistrationMove SKIP\n";
    return 0;
#else
    bool passed = true;
#ifdef USE_EPOLL
    passed = pendingRegistrationMoveCanBeCancelled() && passed;
#endif

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T168] socketpair failed\n";
        return 1;
    }
    if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T168] nonblocking setup failed\n";
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        if (!first_closed || !second_closed) {
            std::cerr << "[T168] socket cleanup failed\n";
        }
        return 1;
    }

    std::atomic<uint64_t> last_error{0};
#ifdef USE_EPOLL
    EpollReactor reactor(8, last_error);
#else
    KqueueReactor reactor(8, last_error);
#endif
    auto start_result = reactor.start();
    if (!start_result) {
        std::cerr << "[T168] reactor start failed\n";
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        if (!first_closed || !second_closed) {
            std::cerr << "[T168] socket cleanup failed\n";
        }
        return 1;
    }

    auto source = std::make_unique<IOController>(GHandle{.fd = fds[0]});
    char recv_buffer = 0;
    RecvAwaitable recv_awaitable(source.get(), &recv_buffer, 1);
    if (!source->fillAwaitable(RECV, &recv_awaitable)) {
        std::cerr << "[T168] failed to attach recv awaitable\n";
        passed = false;
    }

    bool registration_added = false;
    if (passed) {
        const int add_result = reactor.addRecv(source.get());
        if (add_result != 0) {
            std::cerr << "[T168] recv registration failed: " << add_result << '\n';
            passed = false;
        } else if (reactor.flushPendingChanges() != 0) {
            std::cerr << "[T168] registration flush failed\n";
            passed = false;
        } else {
            registration_added = true;
        }
    }

    IOController** owner_slot = nullptr;
    if (passed) {
        owner_slot = source->m_registration_owner_slot;
        if (owner_slot == nullptr || *owner_slot != source.get()) {
            std::cerr << "[T168] reactor entry did not bind source controller\n";
            passed = false;
        }
    }

    std::unique_ptr<IOController> moved;
    if (passed) {
        auto* const stable_controller = source.get();
        moved = std::move(source);
        source.reset();
        if (moved->m_registration_owner_slot != owner_slot ||
            source || moved.get() != stable_controller || *owner_slot != stable_controller ||
            recv_awaitable.m_controller != stable_controller ||
            moved->m_awaitable[IOController::READ] != &recv_awaitable) {
            std::cerr << "[T168] flushed owner transfer changed published identities\n";
            passed = false;
        }
    }

    if (passed) {
        const char byte = 'x';
        ssize_t written = -1;
        do {
            written = ::send(fds[1], &byte, 1, 0);
        } while (written < 0 && errno == EINTR);
        if (written != 1) {
            std::cerr << "[T168] failed to trigger readable event\n";
            passed = false;
        }
    }

    if (passed) {
        std::atomic<bool> sleeping{false};
        std::atomic<bool> wakeup_pending{false};
        WakeCoordinator wake_coordinator(sleeping, wakeup_pending);
#ifdef USE_EPOLL
        reactor.poll(0, wake_coordinator);
#else
        const timespec timeout{0, 0};
        reactor.poll(timeout, wake_coordinator);
#endif
        if (*owner_slot != moved.get() || !recv_awaitable.m_result ||
            recv_awaitable.m_result.value() != 1 || recv_buffer != 'x' ||
            moved->m_awaitable[IOController::READ] != nullptr) {
            std::cerr << "[T168] event dispatch lost the stable controller or recv result\n";
            passed = false;
        }
    }

    IOController* registered_controller = moved ? moved.get() : source.get();
    if (registration_added && registered_controller != nullptr) {
        if (reactor.remove(registered_controller) != 0) {
            std::cerr << "[T168] reactor remove failed\n";
            passed = false;
        }
#ifdef USE_EPOLL
        if (reactor.flushPendingChanges() != 0) {
            std::cerr << "[T168] remove flush failed\n";
            passed = false;
        }
#endif
    }

    if (registered_controller != nullptr) {
        if (reactor.addClose(registered_controller) != 0) {
            std::cerr << "[T168] controller close failed\n";
            passed = false;
        } else {
            fds[0] = -1;
        }
    }
    const bool first_closed = closeFd(fds[0]);
    const bool second_closed = closeFd(fds[1]);
    if (!first_closed || !second_closed || last_error.load() != 0) {
        std::cerr << "[T168] socket close failed\n";
        passed = false;
    }
    if (!passed) {
        return 1;
    }
    std::cout << "T168-RegistrationMove PASS\n";
    return 0;
#endif
}
