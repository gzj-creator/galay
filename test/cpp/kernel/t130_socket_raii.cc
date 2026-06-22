/**
 * @file t130_socket_raii.cc
 * @brief 验证 TcpSocket 对已拥有 fd 的 RAII 关闭语义。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>

#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

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
        galay::async::TcpSocket socket(GHandle{.fd = fd});
    }

    return check(isClosed(fd), "TcpSocket destructor should close the owned fd");
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
        galay::async::TcpSocket target(GHandle{.fd = oldFd});
        galay::async::TcpSocket source(GHandle{.fd = newFd});
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

} // namespace

int main()
{
    bool ok = true;
    ok = destructorClosesOwnedSocket() && ok;
    ok = moveAssignmentClosesPreviousSocketAndTransfersNewOne() && ok;
    return ok ? 0 : 1;
}
