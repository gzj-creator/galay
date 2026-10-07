#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cerrno>
#include <iostream>

namespace
{

constexpr const char* kPath = "/tmp/galay_b22_sendfile_progress.dat";
constexpr size_t kFileSize = 8 * 1024 * 1024;
constexpr int kIterations = 8;

bool close_fd(int fd)
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

bool set_non_block(int fd)
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

bool set_small_send_buffer(int fd)
{
    int size = 4096;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0) {
        return true;
    }
    std::cerr << "setsockopt(SO_SNDBUF) failed errno=" << errno << "\n";
    return false;
}

unsigned char pattern_at(size_t index)
{
    return static_cast<unsigned char>((index * 131u + 17u) & 0xffu);
}

bool create_pattern_file()
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
            buffer[i] = pattern_at(written_total + i);
        }
        size_t written = 0;
        while (written < n) {
            const ssize_t rc = ::write(fd, buffer.data() + written, n - written);
            if (rc <= 0) {
                std::cerr << "write file failed rc=" << rc << " errno=" << errno << "\n";
                const bool closed = close_fd(fd);
                return closed && false;
            }
            written += static_cast<size_t>(rc);
            written_total += static_cast<size_t>(rc);
        }
    }

    return close_fd(fd);
}

bool drain_receiver(int fd, size_t& received)
{
    std::array<unsigned char, 16384> buffer{};
    while (true) {
        const ssize_t rc = ::read(fd, buffer.data(), buffer.size());
        if (rc > 0) {
            const size_t n = static_cast<size_t>(rc);
            for (size_t i = 0; i < n; ++i) {
                if (buffer[i] != pattern_at(received + i)) {
                    std::cerr << "data mismatch at offset=" << (received + i) << "\n";
                    return false;
                }
            }
            received += n;
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

bool run_one_iteration()
{
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        std::cerr << "socketpair failed errno=" << errno << "\n";
        return false;
    }

    bool ok = set_non_block(sockets[0]);
    ok = set_non_block(sockets[1]) && ok;
    ok = set_small_send_buffer(sockets[0]) && ok;

    int file_fd = ::open(kPath, O_RDONLY);
    if (file_fd < 0) {
        std::cerr << "open read file failed errno=" << errno << "\n";
        ok = false;
    }

    size_t received = 0;
    if (ok) {
        galay::kernel::SendFileIOContext context(file_fd, 0, kFileSize);
        bool completed = false;
        for (int attempt = 0; attempt < 8192; ++attempt) {
#ifdef USE_IOURING
            struct io_uring_cqe cqe{};
            cqe.res = 1;
            completed = context.handle_complete(&cqe, GHandle{.fd = sockets[0]});
#else
            completed = context.handle_complete(GHandle{.fd = sockets[0]});
#endif
            if (!drain_receiver(sockets[1], received)) {
                ok = false;
                break;
            }
            if (completed) {
                break;
            }
        }
        ok = completed && context.m_result && context.m_result.value() == kFileSize && ok;
    }

    ok = close_fd(file_fd) && ok;
    ok = close_fd(sockets[0]) && ok;
    if (!drain_receiver(sockets[1], received)) {
        ok = false;
    }
    ok = close_fd(sockets[1]) && ok;

    if (received != kFileSize) {
        std::cerr << "received size mismatch actual=" << received
                  << " expected=" << kFileSize << "\n";
        return false;
    }
    return ok;
}

} // namespace

int main()
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    if (!create_pattern_file()) {
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        if (!run_one_iteration()) {
            return 1;
        }
    }
    const auto finished = std::chrono::steady_clock::now();

    if (::unlink(kPath) != 0 && errno != ENOENT) {
        std::cerr << "unlink failed errno=" << errno << "\n";
        return 1;
    }

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    if (elapsed_ns <= 0) {
        std::cerr << "invalid elapsed time\n";
        return 1;
    }

    const double seconds = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
    const double mb = static_cast<double>(kFileSize * kIterations) / (1024.0 * 1024.0);
    std::cout << "Sendfile one-shot progress benchmark\n";
    std::cout << "  iterations: " << kIterations << "\n";
    std::cout << "  bytes_per_iteration: " << kFileSize << "\n";
    std::cout << "  elapsed_seconds: " << seconds << "\n";
    std::cout << "  throughput_mib_per_sec: " << (mb / seconds) << "\n";
    return 0;
}
