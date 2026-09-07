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

galay::kernel::Task<void> closeInvalidSocket(galay::async::AsyncTcpSocket* socket)
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

int makeSocketFd()
{
    return ::socket(AF_INET, SOCK_STREAM, 0);
}

bool isClosed(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

bool destructorClosesOwnedSocket()
{
    const int fd = makeSocketFd();
    if (!check(fd >= 0, "socket() should create a test fd")) {
        return false;
    }

    {
        galay::async::AsyncTcpSocket socket(GHandle{.fd = fd});
    }

    return check(isClosed(fd), "AsyncTcpSocket destructor should close the owned fd");
}

bool moveAssignmentClosesPreviousSocketAndTransfersNewOne()
{
    const int oldFd = makeSocketFd();
    const int newFd = makeSocketFd();
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

        if (!check(isClosed(oldFd), "move assignment should close the replaced fd")) {
            return false;
        }
        if (!check(!isClosed(newFd), "moved fd should stay owned by the destination")) {
            return false;
        }
    }

    return check(isClosed(newFd), "destination destructor should close the moved fd");
}

bool cloneKeepsSocketAliveUntilLastOwner()
{
    const int fd = makeSocketFd();
    if (!check(fd >= 0, "socket() should create a clone test fd")) {
        return false;
    }

    {
        galay::async::AsyncTcpSocket original(GHandle{.fd = fd});
        auto clone = original.clone();
        if (!check(original.getSharedCount() == 2 && clone.getSharedCount() == 2,
                   "clone should share the controller ownership")) {
            return false;
        }
        original = galay::async::AsyncTcpSocket(GHandle::invalid());
        if (!check(!isClosed(fd), "destroying one clone owner must keep the fd open")) {
            return false;
        }
    }

    return check(isClosed(fd), "last clone owner should close the fd");
}

bool unawaitedCloseClosesOwnedSocket()
{
    const int fd = makeSocketFd();
    if (!check(fd >= 0, "socket() should create an unawaited close test fd")) {
        return false;
    }

    {
        galay::async::AsyncTcpSocket socket(GHandle{.fd = fd});
        auto close_request = socket.close();
        (void)close_request;
    }

    return check(isClosed(fd), "destroying an unawaited close request should close the fd");
}

bool closeReportsClosedSocket()
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
    if (!galay::kernel::scheduleTask(scheduler, closeInvalidSocket(&socket))) {
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
    ok = destructorClosesOwnedSocket() && ok;
    ok = moveAssignmentClosesPreviousSocketAndTransfersNewOne() && ok;
    ok = cloneKeepsSocketAliveUntilLastOwner() && ok;
    ok = unawaitedCloseClosesOwnedSocket() && ok;
    ok = closeReportsClosedSocket() && ok;
    return ok ? 0 : 1;
}
