/**
 * @file t131_udp_file_raii.cc
 * @brief 验证 UdpSocket/AsyncFile 对已拥有 fd 的 RAII 关闭语义。
 */

#include <galay/cpp/galay-kernel/async/udp_socket.h>

#if defined(USE_KQUEUE) || defined(USE_IOURING)
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace {

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
        galay::async::UdpSocket socket(GHandle{.fd = fd});
    }

    return check(isClosed(fd), "UdpSocket destructor should close the owned fd");
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
        galay::async::UdpSocket target(GHandle{.fd = oldFd});
        galay::async::UdpSocket source(GHandle{.fd = newFd});
        target = std::move(source);

        if (!check(isClosed(oldFd), "UdpSocket move assignment should close the replaced fd")) {
            return false;
        }
        if (!check(!isClosed(newFd), "moved UDP fd should stay owned by destination")) {
            return false;
        }
    }

    return check(isClosed(newFd), "UdpSocket destination destructor should close the moved fd");
}

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

bool asyncFileFailedReopenKeepsExistingFd()
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
        auto reopened = file.open(missing_path.string(), galay::async::FileOpenMode::Read);
        ok = check(!reopened.has_value(), "AsyncFile reopen of missing path should fail") && ok;
        ok = check(!isClosed(fd), "failed AsyncFile reopen should keep the existing fd") && ok;
    }

    ok = check(isClosed(fd), "AsyncFile destructor should close fd kept after failed reopen") && ok;
    std::filesystem::remove(path);
    return ok;
}

#endif

} // namespace

int main()
{
    bool ok = true;
    ok = udpDestructorClosesOwnedSocket() && ok;
    ok = udpMoveAssignmentClosesPreviousSocketAndTransfersNewOne() && ok;

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    ok = asyncFileDestructorClosesOwnedFd() && ok;
    ok = asyncFileMoveAssignmentClosesPreviousFdAndTransfersNewOne() && ok;
    ok = asyncFileFailedReopenKeepsExistingFd() && ok;
#endif

    return ok ? 0 : 1;
}
