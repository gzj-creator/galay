/**
 * @file t12_fanout.cc
 * @brief 用途：锁定同一 IO scheduler 下 SslSocket 仅做 connect/accept 时，所有 connect 也必须恢复完成。
 * 关键覆盖点：`SslSocket::connect()` 在已初始化 SSL engine 的前提下不会影响底层 TCP 建连 fanout。
 * 通过条件：服务端 accept 全部完成，客户端 connect 全部完成且无超时。
 */

#include "ssl/async/ssl_socket.h"
#include "ssl/ssl/ssl_context.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef USE_IOURING
#include <kernel/kernel/io_uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <kernel/kernel/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include <kernel/kernel/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

constexpr uint16_t kPort = 19448;
constexpr int kConnections = 16;

struct TestState {
    std::atomic<bool> server_ready{false};
    std::atomic<int> accepted{0};
    std::atomic<int> connected{0};
    std::atomic<int> client_done{0};
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

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Task<void> runServer(SslContext* ctx, TestState* state)
{
    SslSocket listener(ctx);
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    if (!listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort))) {
        fail(state, "bind failed");
        co_return;
    }
    if (!listener.listen(64)) {
        fail(state, "listen failed");
        co_return;
    }

    state->server_ready.store(true, std::memory_order_release);

    for (int i = 0; i < kConnections; ++i) {
        Host client_host;
        auto accepted = co_await listener.accept(&client_host);
        if (!accepted) {
            fail(state, "accept failed");
            break;
        }
        state->accepted.fetch_add(1, std::memory_order_relaxed);

        SslSocket peer(ctx, accepted.value());
        (void)co_await peer.close();
    }

    (void)co_await listener.close();
}

Task<void> runClient(SslContext* ctx, TestState* state)
{
    SslSocket socket(ctx);
    socket.option().handleNonBlock();

    auto connected = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connected) {
        fail(state, "connect failed");
    } else {
        state->connected.fetch_add(1, std::memory_order_relaxed);
    }

    (void)co_await socket.close();
    state->client_done.fetch_add(1, std::memory_order_relaxed);
}

void waitFor(std::atomic<bool>& flag, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void waitForClients(TestState& state)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (state.client_done.load(std::memory_order_relaxed) < kConnections) {
        if (state.failed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state.failure_mu);
            throw std::runtime_error(state.failure.empty() ? "ssl connect fanout failed" : state.failure);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "ssl connect fanout timed out [client_done=" +
                std::to_string(state.client_done.load(std::memory_order_relaxed)) +
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
    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    expect(server_ctx.isValid(), "server context invalid");
    expect(client_ctx.isValid(), "client context invalid");

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    expect(scheduleTask(scheduler, runServer(&server_ctx, &state)), "schedule server failed");
    waitFor(state.server_ready, "server did not become ready");

    for (int i = 0; i < kConnections; ++i) {
        expect(scheduleTask(scheduler, runClient(&client_ctx, &state)), "schedule client failed");
    }

    int rc = 0;
    try {
        waitForClients(state);
    } catch (const std::exception& ex) {
        std::cerr << "[T12] " << ex.what() << "\n";
        rc = 1;
    }

    scheduler.stop();

    if (rc != 0) {
        return rc;
    }
    if (state.connected.load(std::memory_order_relaxed) != kConnections ||
        state.accepted.load(std::memory_order_relaxed) != kConnections) {
        std::cerr << "[T12] count mismatch [connected="
                  << state.connected.load(std::memory_order_relaxed)
                  << ", accepted=" << state.accepted.load(std::memory_order_relaxed) << "]\n";
        return 1;
    }

    std::cout << "t12_fanout PASS\n";
    return 0;
}
