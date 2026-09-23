/**
 * @file t131_udp_file_raii.cc
 * @brief 验证 AsyncUdpSocket/AsyncFile 对已拥有 fd 的 RAII 关闭语义。
 */

#include <galay/cpp/galay-kernel/async/async_udp.h>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/async/async_aio.h>
#endif

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

#if defined(USE_KQUEUE) || defined(USE_IOURING)
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::atomic<bool> g_udp_close_done{false};
std::atomic<bool> g_udp_close_is_closed{false};

galay::kernel::Task<void> closeInvalidUdpSocket(galay::async::AsyncUdpSocket* socket)
{
    auto result = co_await socket->close();
    g_udp_close_is_closed.store(!result && result.error().code() == galay::kernel::kClosed,
                                std::memory_order_release);
    g_udp_close_done.store(true, std::memory_order_release);
}

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t131] " << message << '\n';
    }
    return condition;
}

bool isClosed(int fd)
{
    errno = 0;
    return ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

int makeUdpFd()
{
    return ::socket(AF_INET, SOCK_DGRAM, 0);
}

bool udpDestructorClosesOwnedSocket()
{
    const int fd = makeUdpFd();
    if (!check(fd >= 0, "socket() should create a UDP fd")) {
        return false;
    }

    {
        galay::async::AsyncUdpSocket socket(GHandle{.fd = fd});
    }

    return check(isClosed(fd), "AsyncUdpSocket destructor should close the owned fd");
}

bool udpMoveAssignmentClosesPreviousSocketAndTransfersNewOne()
{
    const int oldFd = makeUdpFd();
    const int newFd = makeUdpFd();
    if (!check(oldFd >= 0 && newFd >= 0, "socket() should create both UDP fds")) {
        if (oldFd >= 0) {
            ::close(oldFd);
        }
        if (newFd >= 0) {
            ::close(newFd);
        }
        return false;
    }

    {
        galay::async::AsyncUdpSocket target(GHandle{.fd = oldFd});
        galay::async::AsyncUdpSocket source(GHandle{.fd = newFd});
        target = std::move(source);

        if (!check(isClosed(oldFd), "AsyncUdpSocket move assignment should close the replaced fd")) {
            return false;
        }
        if (!check(!isClosed(newFd), "moved UDP fd should stay owned by destination")) {
            return false;
        }
    }

    return check(isClosed(newFd), "AsyncUdpSocket destination destructor should close the moved fd");
}

bool udpMovedFromBindReportsClosed()
{
    const int fd = makeUdpFd();
    if (!check(fd >= 0, "socket() should create a moved-from UDP bind fd")) {
        return false;
    }

    galay::async::AsyncUdpSocket socket(GHandle{.fd = fd});
    galay::async::AsyncUdpSocket moved(std::move(socket));
    const galay::kernel::Host host(galay::kernel::IPType::IPV4, "127.0.0.1", 0);
    const auto result = socket.bind(host);
    return check(!result && result.error().code() == galay::kernel::kClosed,
                 "moved-from UDP bind should return kClosed");
}

bool udpCloneKeepsSocketAliveUntilLastOwner()
{
    const int fd = makeUdpFd();
    if (!check(fd >= 0, "socket() should create a UDP clone test fd")) {
        return false;
    }

    {
        galay::async::AsyncUdpSocket original(GHandle{.fd = fd});
        const auto& const_original = original;
        auto clone = const_original.clone();
        if (!check(original.getSharedCount() == 2 && clone.getSharedCount() == 2,
                   "UDP clone should share the controller ownership")) {
            return false;
        }
        original = galay::async::AsyncUdpSocket(GHandle::invalid());
        if (!check(!isClosed(fd), "destroying one UDP clone owner must keep the fd open")) {
            return false;
        }
    }

    return check(isClosed(fd), "last UDP clone owner should close the fd");
}

bool udpUnawaitedCloseClosesOwnedSocket()
{
    const int fd = makeUdpFd();
    if (!check(fd >= 0, "socket() should create a UDP unawaited close test fd")) {
        return false;
    }

    {
        galay::async::AsyncUdpSocket socket(GHandle{.fd = fd});
        auto close_request = socket.close();
        (void)close_request;
    }

    return check(isClosed(fd), "destroying an unawaited UDP close request should close the fd");
}

bool udpCloseReportsClosedSocket()
{
#if defined(USE_EPOLL) || defined(USE_IOURING) || defined(USE_KQUEUE)
    galay::async::AsyncUdpSocket socket(GHandle::invalid());
    TestScheduler scheduler;
    auto started = scheduler.start();
    if (!check(started.has_value(), "scheduler should start for UDP close test")) {
        return false;
    }
    g_udp_close_done.store(false, std::memory_order_release);
    g_udp_close_is_closed.store(false, std::memory_order_release);
    if (!galay::kernel::scheduleTask(scheduler, closeInvalidUdpSocket(&socket))) {
        scheduler.stop();
        return false;
    }
    for (int i = 0; i < 100 && !g_udp_close_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    scheduler.stop();
    return check(g_udp_close_done.load(std::memory_order_acquire) &&
                     g_udp_close_is_closed.load(std::memory_order_acquire),
                 "UDP close on an invalid socket should return kClosed");
#else
    return true;
#endif
}

#ifdef USE_EPOLL

bool asyncAioOpenRejectsWhenAlreadyOpen()
{
    const auto firstPath = std::filesystem::temp_directory_path() /
                           "galay_async_aio_raii_first.tmp";
    const auto secondPath = std::filesystem::temp_directory_path() /
                            "galay_async_aio_raii_second.tmp";
    const auto missingPath = std::filesystem::temp_directory_path() /
                             "galay_async_aio_raii_missing.tmp";
    std::filesystem::remove(missingPath);
    {
        std::ofstream first(firstPath);
        std::ofstream second(secondPath);
        if (!check(first.good() && second.good(), "should create AsyncAio reopen files")) {
            std::filesystem::remove(firstPath);
            std::filesystem::remove(secondPath);
            return false;
        }
        first << "first";
        second << "second";
    }

    bool ok = true;
    int oldFd = -1;
    {
        galay::async::AsyncAio file;
        auto opened = file.open(firstPath.string(), galay::async::AioOpenMode::Read);
        if (!check(opened.has_value(), "AsyncAio initial open should succeed")) {
            std::filesystem::remove(firstPath);
            std::filesystem::remove(secondPath);
            return false;
        }
        oldFd = file.handle().fd;

        {
            galay::async::AsyncAio fresh;
            auto failed = fresh.open(missingPath.string(), galay::async::AioOpenMode::Read);
            ok = check(!failed.has_value(), "AsyncAio open of missing path should fail") && ok;
            ok = check(galay::kernel::IOError::contains(failed.error().code(),
                                                        galay::kernel::kOpenFailed),
                       "AsyncAio open of missing path should report kOpenFailed") && ok;
        }

        auto reopened = file.open(secondPath.string(), galay::async::AioOpenMode::Read);
        ok = check(!reopened.has_value(),
                   "AsyncAio reopen while already open should fail") && ok;
        ok = check(galay::kernel::IOError::contains(reopened.error().code(),
                                                    galay::kernel::kAlreadyOpen),
                   "AsyncAio reopen while already open should report kAlreadyOpen") && ok;
        ok = check(!isClosed(oldFd),
                   "rejected AsyncAio reopen should keep the existing fd") && ok;
    }

    ok = check(isClosed(oldFd), "AsyncAio destructor should close the held fd") && ok;
    std::filesystem::remove(firstPath);
    std::filesystem::remove(secondPath);
    return ok;
}

bool asyncAioClosedMetadataReportsClosed()
{
    galay::async::AsyncAio file;
    const auto invalidSize = file.size();
    const auto invalidSync = file.sync();
    bool ok = check(!invalidSize && invalidSize.error().code() == galay::kernel::kClosed,
                    "AsyncAio size on an invalid fd should return kClosed");
    ok = check(!invalidSync && invalidSync.error().code() == galay::kernel::kClosed,
               "AsyncAio sync on an invalid fd should return kClosed") && ok;
    return ok;
}

#endif

#if defined(USE_KQUEUE) || defined(USE_IOURING)

std::filesystem::path tempFilePath(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

bool createTempFile(const std::filesystem::path& path)
{
    std::ofstream out(path);
    out << "galay";
    return out.good();
}

bool asyncFileDestructorClosesOwnedFd()
{
    const auto path = tempFilePath("galay_async_file_raii_destructor.tmp");
    if (!check(createTempFile(path), "should create temp file")) {
        return false;
    }

    int fd = -1;
    {
        galay::async::AsyncFile file;
        auto opened = file.open(path.string(), galay::async::FileOpenMode::Read);
        if (!check(opened.has_value(), "AsyncFile open should succeed")) {
            std::filesystem::remove(path);
            return false;
        }
        fd = file.handle().fd;
        if (!check(fd >= 0, "AsyncFile should expose a valid fd")) {
            std::filesystem::remove(path);
            return false;
        }
    }

    const bool closed = check(isClosed(fd), "AsyncFile destructor should close the owned fd");
    std::filesystem::remove(path);
    return closed;
}

bool asyncFileMoveAssignmentClosesPreviousFdAndTransfersNewOne()
{
    const auto oldPath = tempFilePath("galay_async_file_raii_old.tmp");
    const auto newPath = tempFilePath("galay_async_file_raii_new.tmp");
    if (!check(createTempFile(oldPath) && createTempFile(newPath), "should create temp files")) {
        return false;
    }

    int oldFd = -1;
    int newFd = -1;
    {
        galay::async::AsyncFile target;
        galay::async::AsyncFile source;
        auto oldOpened = target.open(oldPath.string(), galay::async::FileOpenMode::Read);
        auto newOpened = source.open(newPath.string(), galay::async::FileOpenMode::Read);
        if (!check(oldOpened.has_value() && newOpened.has_value(), "AsyncFile opens should succeed")) {
            std::filesystem::remove(oldPath);
            std::filesystem::remove(newPath);
            return false;
        }
        oldFd = target.handle().fd;
        newFd = source.handle().fd;
        target = std::move(source);

        if (!check(isClosed(oldFd), "AsyncFile move assignment should close the replaced fd")) {
            return false;
        }
        if (!check(!isClosed(newFd), "moved file fd should stay owned by destination")) {
            return false;
        }
    }

    const bool closed = check(isClosed(newFd), "AsyncFile destination destructor should close the moved fd");
    std::filesystem::remove(oldPath);
    std::filesystem::remove(newPath);
    return closed;
}

bool asyncFileOpenRejectsWhenAlreadyOpen()
{
    const auto path = tempFilePath("galay_async_file_raii_reopen.tmp");
    const auto missing_path = tempFilePath("galay_async_file_raii_missing.tmp");
    std::filesystem::remove(missing_path);
    if (!check(createTempFile(path), "should create reopen temp file")) {
        return false;
    }

    bool ok = true;
    int fd = -1;
    {
        galay::async::AsyncFile file;
        auto opened = file.open(path.string(), galay::async::FileOpenMode::Read);
        if (!check(opened.has_value(), "AsyncFile initial open should succeed")) {
            std::filesystem::remove(path);
            return false;
        }
        fd = file.handle().fd;

        {
            galay::async::AsyncFile fresh;
            auto failed = fresh.open(missing_path.string(), galay::async::FileOpenMode::Read);
            ok = check(!failed.has_value(), "AsyncFile open of missing path should fail") && ok;
            ok = check(galay::kernel::IOError::contains(failed.error().code(),
                                                        galay::kernel::kOpenFailed),
                       "AsyncFile open of missing path should report kOpenFailed") && ok;
        }

        auto reopened = file.open(path.string(), galay::async::FileOpenMode::Read);
        ok = check(!reopened.has_value(),
                   "AsyncFile open while already open should fail") && ok;
        ok = check(galay::kernel::IOError::contains(reopened.error().code(),
                                                    galay::kernel::kAlreadyOpen),
                   "AsyncFile open while already open should report kAlreadyOpen") && ok;
        ok = check(!isClosed(fd), "rejected AsyncFile open should keep the existing fd") && ok;
    }

    ok = check(isClosed(fd), "AsyncFile destructor should close the held fd") && ok;
    std::filesystem::remove(path);
    return ok;
}

bool asyncFileClosedMetadataReportsClosed()
{
    galay::async::AsyncFile file;
    const auto invalidSize = file.size();
    const auto invalidSync = file.sync();
    bool ok = check(!invalidSize && invalidSize.error().code() == galay::kernel::kClosed,
                    "AsyncFile size on an invalid fd should return kClosed");
    ok = check(!invalidSync && invalidSync.error().code() == galay::kernel::kClosed,
               "AsyncFile sync on an invalid fd should return kClosed") && ok;
    return ok;
}

#endif

} // namespace

int main()
{
    bool ok = true;
    ok = udpDestructorClosesOwnedSocket() && ok;
    ok = udpMoveAssignmentClosesPreviousSocketAndTransfersNewOne() && ok;
    ok = udpMovedFromBindReportsClosed() && ok;
    ok = udpCloneKeepsSocketAliveUntilLastOwner() && ok;
    ok = udpUnawaitedCloseClosesOwnedSocket() && ok;
    ok = udpCloseReportsClosedSocket() && ok;

#ifdef USE_EPOLL
    ok = asyncAioOpenRejectsWhenAlreadyOpen() && ok;
    ok = asyncAioClosedMetadataReportsClosed() && ok;
#endif

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    ok = asyncFileDestructorClosesOwnedFd() && ok;
    ok = asyncFileMoveAssignmentClosesPreviousFdAndTransfersNewOne() && ok;
    ok = asyncFileOpenRejectsWhenAlreadyOpen() && ok;
    ok = asyncFileClosedMetadataReportsClosed() && ok;
#endif

    return ok ? 0 : 1;
}
