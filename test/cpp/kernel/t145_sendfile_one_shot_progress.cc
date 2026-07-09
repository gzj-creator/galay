#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{

constexpr const char* kPath = "/tmp/galay_t145_sendfile_progress.dat";
constexpr size_t kFileSize = 4 * 1024 * 1024;

bool closeFd(int fd)
{
    if (fd < 0) {
        return true;
    }
    if (::close(fd) == 0) {
        return true;
    }
    std::cerr << "close failed errno=" << errno << "\n";
    return false;
}

bool setNonBlock(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "fcntl(F_GETFL) failed errno=" << errno << "\n";
        return false;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        return true;
    }
    std::cerr << "fcntl(F_SETFL) failed errno=" << errno << "\n";
    return false;
}

bool setSmallSendBuffer(int fd)
{
    int size = 4096;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0) {
        return true;
    }
    std::cerr << "setsockopt(SO_SNDBUF) failed errno=" << errno << "\n";
    return false;
}

unsigned char patternAt(size_t index)
{
    return static_cast<unsigned char>((index * 131u + 17u) & 0xffu);
}

bool createPatternFile()
{
    int fd = ::open(kPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        std::cerr << "open write file failed errno=" << errno << "\n";
        return false;
    }

    std::array<unsigned char, 8192> buffer{};
    size_t written_total = 0;
    while (written_total < kFileSize) {
        const size_t n = std::min(buffer.size(), kFileSize - written_total);
        for (size_t i = 0; i < n; ++i) {
            buffer[i] = patternAt(written_total + i);
        }
        size_t written = 0;
        while (written < n) {
            const ssize_t rc = ::write(fd, buffer.data() + written, n - written);
            if (rc < 0) {
                std::cerr << "write file failed errno=" << errno << "\n";
                const bool closed = closeFd(fd);
                return closed && false;
            }
            if (rc == 0) {
                std::cerr << "write file made no progress\n";
                const bool closed = closeFd(fd);
                return closed && false;
            }
            written += static_cast<size_t>(rc);
            written_total += static_cast<size_t>(rc);
        }
    }

    return closeFd(fd);
}

bool drainReceiver(int fd, std::vector<unsigned char>& out)
{
    std::array<unsigned char, 16384> buffer{};
    while (true) {
        const ssize_t rc = ::read(fd, buffer.data(), buffer.size());
        if (rc > 0) {
            out.insert(out.end(), buffer.begin(), buffer.begin() + rc);
            continue;
        }
        if (rc == 0) {
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        std::cerr << "read receiver failed errno=" << errno << "\n";
        return false;
    }
}

bool verifyPattern(const std::vector<unsigned char>& data)
{
    if (data.size() != kFileSize) {
        std::cerr << "received size mismatch actual=" << data.size()
                  << " expected=" << kFileSize << "\n";
        return false;
    }
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != patternAt(i)) {
            std::cerr << "received data mismatch at offset=" << i << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    if (!createPatternFile()) {
        return 1;
    }

    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        std::cerr << "socketpair failed errno=" << errno << "\n";
        return 1;
    }

    bool ok = setNonBlock(sockets[0]);
    ok = setNonBlock(sockets[1]) && ok;
    ok = setSmallSendBuffer(sockets[0]) && ok;
    if (!ok) {
        const bool closed0 = closeFd(sockets[0]);
        const bool closed1 = closeFd(sockets[1]);
        return (closed0 && closed1) ? 1 : 1;
    }

    int file_fd = ::open(kPath, O_RDONLY);
    if (file_fd < 0) {
        std::cerr << "open read file failed errno=" << errno << "\n";
        const bool closed0 = closeFd(sockets[0]);
        const bool closed1 = closeFd(sockets[1]);
        return (closed0 && closed1) ? 1 : 1;
    }

    galay::kernel::SendFileIOContext context(file_fd, 0, kFileSize);
    std::vector<unsigned char> received;
    received.reserve(kFileSize);

    bool saw_not_ready = false;
    bool completed = false;
    for (int attempt = 0; attempt < 4096; ++attempt) {
#ifdef USE_IOURING
        struct io_uring_cqe cqe{};
        cqe.res = 1;
        completed = context.handleComplete(&cqe, GHandle{.fd = sockets[0]});
#else
        completed = context.handleComplete(GHandle{.fd = sockets[0]});
#endif
        if (!completed) {
            saw_not_ready = true;
        }
        if (!drainReceiver(sockets[1], received)) {
            ok = false;
            break;
        }
        if (completed) {
            break;
        }
    }

    ok = closeFd(file_fd) && ok;
    ok = closeFd(sockets[0]) && ok;
    if (!drainReceiver(sockets[1], received)) {
        ok = false;
    }
    ok = closeFd(sockets[1]) && ok;

    if (!completed || !context.m_result || context.m_result.value() != kFileSize) {
        std::cerr << "sendfile context did not complete whole request completed="
                  << completed << " transferred="
                  << (context.m_result ? context.m_result.value() : 0) << "\n";
        return 1;
    }
    if (!saw_not_ready) {
        std::cerr << "test did not exercise kNotReady retry path\n";
        return 1;
    }
    if (!ok || !verifyPattern(received)) {
        return 1;
    }

    if (::unlink(kPath) != 0 && errno != ENOENT) {
        std::cerr << "unlink failed errno=" << errno << "\n";
        return 1;
    }

    std::cout << "T145-SendFileOneShotProgress PASS bytes=" << received.size() << "\n";
    return 0;
}
