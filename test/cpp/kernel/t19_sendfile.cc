/**
 * @file t19_sendfile.cc
 * @brief Verifies a bounded AsyncTcpSocket sendfile loopback transfer.
 *
 * This is intentionally a small correctness test. Throughput and large-file
 * coverage belong in benchmark/cpp/kernel/b13_sendfile_throughput.cc.
 */

#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#endif

using galay::async::AsyncTcpSocket;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace
{

constexpr size_t kFileSize = 256 * 1024;
constexpr auto kOperationTimeout = 2s;
constexpr auto kOverallTimeout = 3s;

struct ScopedFd {
    int value = -1;

    ~ScopedFd()
    {
        if (value >= 0) {
            (void)::close(value);
        }
    }
};

struct TemporaryFile {
    ScopedFd fd;
    std::array<char, 64> path{};

    ~TemporaryFile()
    {
        if (path.front() != '\0') {
            (void)::unlink(path.data());
        }
    }
};

struct TestState {
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_ok{false};
    std::atomic<bool> server_done{false};
    std::atomic<bool> server_ok{false};
    std::atomic<size_t> sent_bytes{0};
    std::vector<unsigned char> received;
};

enum class WaitResult {
    kReady,
    kTimeout,
    kError,
};

unsigned char pattern_at(size_t index)
{
    return static_cast<unsigned char>((index * 131U + 17U) & 0xffU);
}

void report_failure(std::string_view stage, std::string_view message)
{
    std::cerr << "[t19][" << stage << "] " << message << '\n';
}

bool write_all(int fd, const unsigned char* data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool create_temporary_file(TemporaryFile* file)
{
    constexpr char kTemplate[] = "/tmp/galay-t19-sendfile-XXXXXX";
    static_assert(sizeof(kTemplate) <= std::tuple_size_v<decltype(file->path)>);
    std::memcpy(file->path.data(), kTemplate, sizeof(kTemplate));

    file->fd.value = ::mkstemp(file->path.data());
    if (file->fd.value < 0) {
        report_failure("setup", "mkstemp failed");
        return false;
    }

    std::array<unsigned char, 8192> buffer{};
    for (size_t offset = 0; offset < kFileSize;) {
        const size_t chunk_size = std::min(buffer.size(), kFileSize - offset);
        for (size_t index = 0; index < chunk_size; ++index) {
            buffer[index] = pattern_at(offset + index);
        }
        if (!write_all(file->fd.value, buffer.data(), chunk_size)) {
            report_failure("setup", "temporary file write failed");
            return false;
        }
        offset += chunk_size;
    }
    return true;
}

bool set_non_blocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool create_listener(ScopedFd* listener, uint16_t* port)
{
    listener->value = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener->value < 0) {
        report_failure("setup", "socket failed");
        return false;
    }

    int reuse = 1;
    if (::setsockopt(listener->value, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        !set_non_blocking(listener->value)) {
        report_failure("setup", "listener option setup failed");
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener->value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener->value, 1) != 0) {
        report_failure("setup", "listener bind or listen failed");
        return false;
    }

    socklen_t address_size = sizeof(address);
    if (::getsockname(listener->value, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
        address.sin_family != AF_INET || address.sin_port == 0) {
        report_failure("setup", "getsockname failed");
        return false;
    }

    *port = ntohs(address.sin_port);
    return true;
}

WaitResult wait_for_input(int fd, std::chrono::steady_clock::time_point deadline)
{
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return WaitResult::kTimeout;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int timeout_ms = static_cast<int>(std::max<int64_t>(1, remaining.count()));
        pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
        const int result = ::poll(&descriptor, 1, timeout_ms);
        if (result > 0) {
            return WaitResult::kReady;
        }
        if (result == 0) {
            return WaitResult::kTimeout;
        }
        if (errno != EINTR) {
            return WaitResult::kError;
        }
    }
}

int accept_within_deadline(int listener, std::chrono::steady_clock::time_point deadline)
{
    while (true) {
        const WaitResult wait_result = wait_for_input(listener, deadline);
        if (wait_result != WaitResult::kReady) {
            return -1;
        }

        const int peer = ::accept(listener, nullptr, nullptr);
        if (peer >= 0) {
            if (!set_non_blocking(peer)) {
                (void)::close(peer);
                return -1;
            }
            return peer;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
}

void receive_file(int listener, TestState* state)
{
    const auto deadline = std::chrono::steady_clock::now() + kOperationTimeout;
    const int peer = accept_within_deadline(listener, deadline);
    if (peer < 0) {
        report_failure("server", "accept timed out or failed");
        state->server_done.store(true, std::memory_order_release);
        return;
    }

    state->received.clear();
    state->received.reserve(kFileSize);
    std::array<unsigned char, 8192> buffer{};
    bool ok = true;
    while (state->received.size() < kFileSize) {
        if (wait_for_input(peer, deadline) != WaitResult::kReady) {
            report_failure("server", "receive timed out or failed");
            ok = false;
            break;
        }

        const ssize_t result = ::recv(peer, buffer.data(), buffer.size(), 0);
        if (result > 0) {
            state->received.insert(state->received.end(), buffer.begin(),
                                   buffer.begin() + static_cast<std::ptrdiff_t>(result));
            if (state->received.size() > kFileSize) {
                report_failure("server", "received more bytes than expected");
                ok = false;
                break;
            }
            continue;
        }
        if (result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        report_failure("server", "peer closed or receive failed before the transfer completed");
        ok = false;
        break;
    }

    (void)::close(peer);
    state->server_ok.store(ok && state->received.size() == kFileSize, std::memory_order_release);
    state->server_done.store(true, std::memory_order_release);
}

Task<void> send_file(TestState* state, int file_fd, uint16_t port)
{
    AsyncTcpSocket socket(IPType::IPV4);
    const auto non_blocking = socket.option().handle_non_block();
    if (!non_blocking) {
        report_failure("client", "failed to enable nonblocking mode");
        state->client_done.store(true, std::memory_order_release);
        co_return;
    }

    const auto connected = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", port)).timeout(kOperationTimeout);
    if (!connected) {
        report_failure("client", "connect failed");
        state->client_done.store(true, std::memory_order_release);
        co_return;
    }

    size_t transferred = 0;
    while (transferred < kFileSize) {
        const auto sent = co_await socket.sendfile(
            file_fd,
            static_cast<off_t>(transferred),
            kFileSize - transferred).timeout(kOperationTimeout);
        if (!sent || sent.value() == 0) {
            report_failure("client", "sendfile failed or made no progress");
            state->client_done.store(true, std::memory_order_release);
            co_return;
        }
        transferred += sent.value();
    }

    const auto closed = co_await socket.close().timeout(kOperationTimeout);
    if (!closed) {
        report_failure("client", "close failed");
        state->client_done.store(true, std::memory_order_release);
        co_return;
    }

    state->sent_bytes.store(transferred, std::memory_order_release);
    state->client_ok.store(true, std::memory_order_release);
    state->client_done.store(true, std::memory_order_release);
}

bool wait_for_client(const TestState& state)
{
    const auto deadline = std::chrono::steady_clock::now() + kOverallTimeout;
    while (!state.client_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

bool verify_pattern(const std::vector<unsigned char>& received)
{
    if (received.size() != kFileSize) {
        return false;
    }
    for (size_t index = 0; index < received.size(); ++index) {
        if (received[index] != pattern_at(index)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    TemporaryFile file;
    if (!create_temporary_file(&file)) {
        return 1;
    }

    ScopedFd listener;
    uint16_t port = 0;
    if (!create_listener(&listener, &port)) {
        return 1;
    }

    TestScheduler scheduler;
    const auto started = scheduler.start();
    if (!started) {
        std::cerr << "[t19] scheduler start failed: " << started.error().message() << '\n';
        return 1;
    }

    TestState state;
    std::thread server(receive_file, listener.value, &state);
    if (!schedule_task(scheduler, send_file(&state, file.fd.value, port))) {
        std::cerr << "[t19] schedule sendfile task failed\n";
        scheduler.stop();
        server.join();
        return 1;
    }

    const bool client_finished = wait_for_client(state);
    scheduler.stop();
    server.join();

    if (!client_finished) {
        std::cerr << "[t19] client task timed out\n";
        return 1;
    }
    if (!state.client_ok.load(std::memory_order_acquire) ||
        !state.server_done.load(std::memory_order_acquire) ||
        !state.server_ok.load(std::memory_order_acquire) ||
        state.sent_bytes.load(std::memory_order_acquire) != kFileSize ||
        !verify_pattern(state.received)) {
        std::cerr << "[t19] sendfile loopback verification failed [sent="
                  << state.sent_bytes.load(std::memory_order_acquire)
                  << ", received=" << state.received.size() << "]\n";
        return 1;
    }

    std::cout << "T19-SendFileLoopback PASS bytes=" << state.received.size() << '\n';
    return 0;
}
