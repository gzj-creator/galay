/**
 * @file io_handlers.hpp
 * @brief 平台相关的 IO 系统调用封装
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供 POSIX 和平台相关 IO 系统调用（accept、recv、send、readv、writev、
 * connect、sendfile 等）的薄内联封装，将错误码转换为 galay::kernel::IOError。
 * 在 io_uring 模式下，封装通过消费完成队列条目（CQE）而非直接执行系统调用。
 */

#ifndef GALAY_KERNEL_IOHANDLERS_HPP
#define GALAY_KERNEL_IOHANDLERS_HPP

#if defined(USE_KQUEUE) || defined(USE_EPOLL)

#include "galay-kernel/common/defn.hpp"
#include "galay-kernel/common/error.h"
#include "galay-kernel/common/host.hpp"
#include <sys/socket.h>
#include <sys/uio.h>
#ifdef USE_EPOLL
#include <sys/sendfile.h>
#endif
#include <unistd.h>
#include <cerrno>
#include <expected>
#include <utility>

namespace galay::kernel::io
{

inline std::pair<std::expected<GHandle, IOError>, Host> handleAccept(GHandle listen_handle)
{
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    GHandle handle {
#if defined(__linux__)
        .fd = accept4(listen_handle.fd,
                      reinterpret_cast<sockaddr*>(&addr),
                      &addr_len,
                      SOCK_NONBLOCK | SOCK_CLOEXEC),
#else
        .fd = accept(listen_handle.fd, reinterpret_cast<sockaddr*>(&addr), &addr_len),
#endif
    };
    if( handle.fd < 0 ) {
        if( static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR ) {
            return {std::unexpected(IOError(kNotReady, 0)), Host{}};
        }
        return {std::unexpected(IOError(kAcceptFailed, static_cast<uint32_t>(errno))), Host{}};
    }
    Host host = Host::fromSockAddr(addr);
    return {handle, std::move(host)};
}

inline std::expected<size_t, IOError> handleRecv(GHandle handle, char* buffer, size_t length)
{
    ssize_t recvBytes = recv(handle.fd, buffer, length, 0);
    if (recvBytes >= 0) {
        return static_cast<size_t>(recvBytes);
    } else {
        if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<size_t, IOError> handleSend(GHandle handle, const char* buffer, size_t length)
{
    ssize_t sentBytes = send(handle.fd, buffer, length, 0);
    if (sentBytes >= 0) {
        return static_cast<size_t>(sentBytes);
    } else {
        if (static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<size_t, IOError> handleReadv(GHandle handle, struct iovec* iovecs, int iovcnt)
{
    ssize_t readBytes = readv(handle.fd, iovecs, iovcnt);
    if (readBytes > 0) {
        return static_cast<size_t>(readBytes);
    } else if (readBytes == 0) {
        return std::unexpected(IOError(kDisconnectError, 0));
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<size_t, IOError> handleWritev(GHandle handle, struct iovec* iovecs, int iovcnt)
{
    ssize_t writtenBytes = writev(handle.fd, iovecs, iovcnt);
    if (writtenBytes >= 0) {
        return static_cast<size_t>(writtenBytes);
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<void, IOError> handleConnect(GHandle handle, const Host& host)
{
    int result = ::connect(handle.fd, host.sockAddr(), host.addrLen());
    if (result == 0) {
        return {};
    } else if (errno == EINPROGRESS) {
        return std::unexpected(IOError(kNotReady, 0));
    } else if (errno == EISCONN) {
        return {};
    } else {
        return std::unexpected(IOError(kConnectFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::pair<std::expected<size_t, IOError>, Host> handleRecvFrom(GHandle handle, char* buffer, size_t length)
{
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    ssize_t recvBytes = recvfrom(handle.fd, buffer, length, 0, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (recvBytes >= 0) {
        Host host = Host::fromSockAddr(addr);
        return {static_cast<size_t>(recvBytes), std::move(host)};
    } else {
        if (static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR) {
            return {std::unexpected(IOError(kNotReady, 0)), Host{}};
        }
        return {std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(errno))), Host{}};
    }
}

inline std::expected<size_t, IOError> handleSendTo(GHandle handle, const char* buffer, size_t length, const Host& to)
{
    ssize_t sentBytes = sendto(handle.fd, buffer, length, 0, to.sockAddr(), to.addrLen());
    if (sentBytes >= 0) {
        return static_cast<size_t>(sentBytes);
    } else {
        if (static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<size_t, IOError> handleFileRead(GHandle handle, char* buffer, size_t length, off_t offset)
{
    ssize_t readBytes = pread(handle.fd, buffer, length, offset);
    if (readBytes >= 0) {
        return static_cast<size_t>(readBytes);
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kReadFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<size_t, IOError> handleFileWrite(GHandle handle, const char* buffer, size_t length, off_t offset)
{
    ssize_t writtenBytes = pwrite(handle.fd, buffer, length, offset);
    if (writtenBytes >= 0) {
        return static_cast<size_t>(writtenBytes);
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kWriteFailed, static_cast<uint32_t>(errno)));
    }
}

inline std::expected<size_t, IOError> handleSendFile(GHandle socket_handle, int file_fd, off_t offset, size_t count)
{
#ifdef USE_KQUEUE
    // macOS: sendfile(in_fd, out_fd, offset, &len, hdtr, flags)
    off_t len = static_cast<off_t>(count);
    int result = ::sendfile(file_fd, socket_handle.fd, offset, &len, nullptr, 0);
    if (result == 0) {
        return static_cast<size_t>(len);
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (len > 0) {
                return static_cast<size_t>(len);
            }
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(errno)));
    }
#elif defined(USE_EPOLL)
    // Linux: sendfile(out_fd, in_fd, &offset, count)
    ssize_t sentBytes = ::sendfile(socket_handle.fd, file_fd, &offset, count);
    if (sentBytes >= 0) {
        return static_cast<size_t>(sentBytes);
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(errno)));
    }
#endif
}

} // namespace galay::kernel::io


#elif defined(USE_IOURING)

#include "galay-kernel/common/defn.hpp"
#include "galay-kernel/common/error.h"
#include "galay-kernel/common/host.hpp"
#include "watch_defs.hpp"
#include <liburing.h>
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif
#include <cerrno>
#include <expected>
#include <sys/inotify.h>
#include <sys/sendfile.h>

namespace galay::kernel::io
{

inline std::expected<GHandle, IOError> handleAccept(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res >= 0) {
        return GHandle{.fd = res};
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kAcceptFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleRecv(struct io_uring_cqe* cqe,
                                                [[maybe_unused]] char* buffer)
{
    int res = cqe->res;
    if (res >= 0) {
        return static_cast<size_t>(res);
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleSend(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res >= 0) {
        return static_cast<size_t>(res);
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleReadv(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res > 0) {
        return static_cast<size_t>(res);
    } else if (res == 0) {
        return std::unexpected(IOError(kDisconnectError, 0));
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleWritev(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res >= 0) {
        return static_cast<size_t>(res);
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<void, IOError> handleConnect(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res == 0) {
        return {};
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kConnectFailed, static_cast<uint32_t>(-res)));
}

inline std::pair<std::expected<size_t, IOError>, Host> handleRecvFrom(
    struct io_uring_cqe* cqe,
    [[maybe_unused]] char* buffer,
    const sockaddr_storage& addr)
{
    int res = cqe->res;
    if (res >= 0) {
        Host host = Host::fromSockAddr(addr);
        return {static_cast<size_t>(res), std::move(host)};
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return {std::unexpected(IOError(kNotReady, 0)), Host{}};
    }
    return {std::unexpected(IOError(kRecvFailed, static_cast<uint32_t>(-res))), Host{}};
}

inline std::expected<size_t, IOError> handleSendTo(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res >= 0) {
        return static_cast<size_t>(res);
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleFileRead(struct io_uring_cqe* cqe,
                                                     [[maybe_unused]] char* buffer)
{
    int res = cqe->res;
    if (res >= 0) {
        return static_cast<size_t>(res);
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kReadFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleFileWrite(struct io_uring_cqe* cqe)
{
    int res = cqe->res;
    if (res >= 0) {
        return static_cast<size_t>(res);
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kWriteFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<FileWatchResult, IOError> handleFileWatch(struct io_uring_cqe* cqe, char* buffer)
{
    int res = cqe->res;
    if (res > 0) {
        struct inotify_event* event = reinterpret_cast<struct inotify_event*>(buffer);
        FileWatchResult result;
        result.isDir = (event->mask & IN_ISDIR) != 0;
        if (event->len > 0) {
            result.name = std::string(event->name);
        }
        uint32_t mask = 0;
        if (event->mask & IN_ACCESS)       mask |= static_cast<uint32_t>(FileWatchEvent::Access);
        if (event->mask & IN_MODIFY)       mask |= static_cast<uint32_t>(FileWatchEvent::Modify);
        if (event->mask & IN_ATTRIB)       mask |= static_cast<uint32_t>(FileWatchEvent::Attrib);
        if (event->mask & IN_CLOSE_WRITE)  mask |= static_cast<uint32_t>(FileWatchEvent::CloseWrite);
        if (event->mask & IN_CLOSE_NOWRITE) mask |= static_cast<uint32_t>(FileWatchEvent::CloseNoWrite);
        if (event->mask & IN_OPEN)         mask |= static_cast<uint32_t>(FileWatchEvent::Open);
        if (event->mask & IN_MOVED_FROM)   mask |= static_cast<uint32_t>(FileWatchEvent::MovedFrom);
        if (event->mask & IN_MOVED_TO)     mask |= static_cast<uint32_t>(FileWatchEvent::MovedTo);
        if (event->mask & IN_CREATE)       mask |= static_cast<uint32_t>(FileWatchEvent::Create);
        if (event->mask & IN_DELETE)       mask |= static_cast<uint32_t>(FileWatchEvent::Delete);
        if (event->mask & IN_DELETE_SELF)  mask |= static_cast<uint32_t>(FileWatchEvent::DeleteSelf);
        if (event->mask & IN_MOVE_SELF)    mask |= static_cast<uint32_t>(FileWatchEvent::MoveSelf);
        result.event = static_cast<FileWatchEvent>(mask);
        return result;
    } else if (res == 0) {
        return std::unexpected(IOError(kReadFailed, 0));
    }
    if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kReadFailed, static_cast<uint32_t>(-res)));
}

inline std::expected<size_t, IOError> handleSendFile(struct io_uring_cqe* cqe, GHandle socket_handle, int file_fd, off_t offset, size_t count)
{
    int res = cqe->res;
    if (res < 0) {
        if (-res == EAGAIN || -res == EWOULDBLOCK || -res == EINTR) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(-res)));
    }
    // poll 完成后，调用 sendfile 系统调用
    ssize_t sentBytes = ::sendfile(socket_handle.fd, file_fd, &offset, count);
    if (sentBytes > 0) {
        return static_cast<size_t>(sentBytes);
    } else if (sentBytes == 0) {
        return static_cast<size_t>(0);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    return std::unexpected(IOError(kSendFailed, static_cast<uint32_t>(errno)));
}

} // namespace galay::kernel::io

#endif // defined(USE_IOURING)

#endif // GALAY_KERNEL_IOHANDLERS_HPP
