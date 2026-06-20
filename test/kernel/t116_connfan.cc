/**
 * @file t116_connfan.cc
 * @brief 用途：锁定并发 nonblocking connect 在同一 IO scheduler 下必须全部恢复完成。
 * 关键覆盖点：`ConnectAwaitable`、后端写就绪完成判定，以及同一监听端并发建连的 fanout 行为。
 * 通过条件：服务端 accept 到全部连接，客户端 connect await 也全部完成且无超时。
 */

#include <galay/cpp/galay-kernel/common/host.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;
using galay::async::TcpSocket;

namespace {

constexpr int kConnections = 16;

struct TestState {
    std::atomic<int> connected{0};
    std::atomic<int> done{0};
    std::atomic<int> accepted{0};
    std::atomic<bool> failed{false};
    std::mutex failure_mu;
    std::string failure;
};

void fail(TestState* state, std::string message)
{
    state->failed.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state->failure_mu);
    if (state->failure.empty()) {
        state->failure = std::move(message);
    }
}

int createListenSocket(uint16_t* port_out)
{
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listen_fd);
        return -1;
    }
    if (::listen(listen_fd, 128) != 0) {
        ::close(listen_fd);
        return -1;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        ::close(listen_fd);
        return -1;
    }

    *port_out = ntohs(bound.sin_port);
    return listen_fd;
}

void acceptAll(int listen_fd, TestState* state)
{
    for (int i = 0; i < kConnections; ++i) {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            fail(state, std::string("accept failed: ") + std::strerror(errno));
            return;
        }
        state->accepted.fetch_add(1, std::memory_order_relaxed);
        ::close(client_fd);
    }
}

Task<void> connectTask(TestState* state, uint16_t port)
{
    TcpSocket socket(IPType::IPV4);
    socket.option().handleNonBlock();

    auto result = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", port));
    if (!result) {
        fail(state, "connect failed");
    } else {
        state->connected.fetch_add(1, std::memory_order_relaxed);
    }

    (void)co_await socket.close();
    state->done.fetch_add(1, std::memory_order_relaxed);
}

void waitForState(TestState& state)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state.done.load(std::memory_order_relaxed) < kConnections) {
        if (state.failed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state.failure_mu);
            throw std::runtime_error(state.failure.empty() ? "concurrent connect failed" : state.failure);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "concurrent connect timed out [done=" +
                std::to_string(state.done.load(std::memory_order_relaxed)) +
                ", connected=" + std::to_string(state.connected.load(std::memory_order_relaxed)) +
                ", accepted=" + std::to_string(state.accepted.load(std::memory_order_relaxed)) + "]"
            );
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main()
{
    uint16_t port = 0;
    int listen_fd = createListenSocket(&port);
    if (listen_fd < 0) {
        std::cerr << "[T116] createListenSocket failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    TestState state;
    std::thread accept_thread(acceptAll, listen_fd, &state);

    TestScheduler scheduler;
    scheduler.start();

    for (int i = 0; i < kConnections; ++i) {
        if (!scheduleTask(scheduler, connectTask(&state, port))) {
            scheduler.stop();
            accept_thread.join();
            ::close(listen_fd);
            std::cerr << "[T116] scheduleTask failed\n";
            return 1;
        }
    }

    int rc = 0;
    try {
        waitForState(state);
    } catch (const std::exception& ex) {
        std::cerr << "[T116] " << ex.what() << "\n";
        rc = 1;
    }

    scheduler.stop();
    accept_thread.join();
    ::close(listen_fd);

    if (rc != 0) {
        return rc;
    }
    if (state.connected.load(std::memory_order_relaxed) != kConnections ||
        state.accepted.load(std::memory_order_relaxed) != kConnections) {
        std::cerr << "[T116] count mismatch [connected="
                  << state.connected.load(std::memory_order_relaxed)
                  << ", accepted=" << state.accepted.load(std::memory_order_relaxed) << "]\n";
        return 1;
    }

    std::cout << "T116-ConcurrentConnectFanout PASS\n";
    return 0;
}
