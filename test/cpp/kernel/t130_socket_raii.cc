/**
 * @file t130_socket_raii.cc
 * @brief 验证 AsyncTcpSocket 对已拥有 fd 的 RAII 关闭语义。
 */

#include <galay/cpp/galay-kernel/async/async_tcp.h>

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace std::chrono_literals;

std::atomic<bool> g_close_done{false};
std::atomic<bool> g_close_is_closed{false};

galay::kernel::Task<void> close_invalid_socket(galay::async::AsyncTcpSocket* socket)
{
    auto result = co_await socket->close();
    g_close_is_closed.store(!result && result.error().code() == galay::kernel::kClosed,
                            std::memory_order_release);
    g_close_done.store(true, std::memory_order_release);
}

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t130] " << message << '\n';
    }
    return condition;
}

int make_socket_fd()
{
    return ::socket(AF_INET, SOCK_STREAM, 0);
}

bool is_closed(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

bool destructor_closes_owned_socket()
{
    const int fd = make_socket_fd();
    if (!check(fd >= 0, "socket() should create a test fd")) {
        return false;
    }

    {
        galay::async::AsyncTcpSocket socket(GHandle{.fd = fd});
    }

    return check(is_closed(fd), "AsyncTcpSocket destructor should close the owned fd");
}

bool move_assignment_closes_previous_socket_and_transfers_new_one()
{
    const int oldFd = make_socket_fd();
    const int newFd = make_socket_fd();
    if (!check(oldFd >= 0 && newFd >= 0, "socket() should create both test fds")) {
        if (oldFd >= 0) {
            ::close(oldFd);
        }
        if (newFd >= 0) {
            ::close(newFd);
        }
        return false;
    }

    {
        galay::async::AsyncTcpSocket target(GHandle{.fd = oldFd});
        galay::async::AsyncTcpSocket source(GHandle{.fd = newFd});
        target = std::move(source);

        if (!check(is_closed(oldFd), "move assignment should close the replaced fd")) {
            return false;
        }
        if (!check(!is_closed(newFd), "moved fd should stay owned by the destination")) {
            return false;
        }
    }

    return check(is_closed(newFd), "destination destructor should close the moved fd");
}

bool moved_from_sync_operations_report_closed()
{
    const int fd = make_socket_fd();
    if (!check(fd >= 0, "socket() should create a moved-from sync operation fd")) {
        return false;
    }

    galay::async::AsyncTcpSocket socket(GHandle{.fd = fd});
    galay::async::AsyncTcpSocket moved(std::move(socket));
    const galay::kernel::Host host(galay::kernel::IPType::IPV4, "127.0.0.1", 0);
    const auto bind_result = socket.bind(host);
    const auto listen_result = socket.listen();
    const bool bind_closed = !bind_result && bind_result.error().code() == galay::kernel::kClosed;
    const bool listen_closed = !listen_result && listen_result.error().code() == galay::kernel::kClosed;
    return check(bind_closed && listen_closed,
                 "moved-from TCP bind/listen should return kClosed");
}

bool clone_keeps_socket_alive_until_last_owner()
{
    const int fd = make_socket_fd();
    if (!check(fd >= 0, "socket() should create a clone test fd")) {
        return false;
    }

    {
        galay::async::AsyncTcpSocket original(GHandle{.fd = fd});
        auto clone = original.clone();
        if (!check(original.get_shared_count() == 2 && clone.get_shared_count() == 2,
                   "clone should share the controller ownership")) {
            return false;
        }
        original = galay::async::AsyncTcpSocket(GHandle::invalid());
        if (!check(!is_closed(fd), "destroying one clone owner must keep the fd open")) {
            return false;
        }
    }

    return check(is_closed(fd), "last clone owner should close the fd");
}

bool unawaited_close_closes_owned_socket()
{
    const int fd = make_socket_fd();
    if (!check(fd >= 0, "socket() should create an unawaited close test fd")) {
        return false;
    }

    {
        galay::async::AsyncTcpSocket socket(GHandle{.fd = fd});
        auto close_request = socket.close();
        (void)close_request;
    }

    return check(is_closed(fd), "destroying an unawaited close request should close the fd");
}

bool close_reports_closed_socket()
{
    galay::async::AsyncTcpSocket socket(GHandle::invalid());
#if defined(USE_EPOLL) || defined(USE_IOURING) || defined(USE_KQUEUE)
    TestScheduler scheduler;
    auto started = scheduler.start();
    if (!check(started.has_value(), "scheduler should start for close test")) {
        return false;
    }
    g_close_done.store(false, std::memory_order_release);
    g_close_is_closed.store(false, std::memory_order_release);
    if (!galay::kernel::schedule_task(scheduler, close_invalid_socket(&socket))) {
        scheduler.stop();
        return false;
    }
    for (int i = 0; i < 100 && !g_close_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    scheduler.stop();
    return check(g_close_done.load(std::memory_order_acquire) &&
                     g_close_is_closed.load(std::memory_order_acquire),
                 "close on an invalid socket should return kClosed");
#else
    return true;
#endif
}

} // namespace

int main()
{
    bool ok = true;
    ok = destructor_closes_owned_socket() && ok;
    ok = move_assignment_closes_previous_socket_and_transfers_new_one() && ok;
    ok = moved_from_sync_operations_report_closed() && ok;
    ok = clone_keeps_socket_alive_until_last_owner() && ok;
    ok = unawaited_close_closes_owned_socket() && ok;
    ok = close_reports_closed_socket() && ok;
    return ok ? 0 : 1;
}
