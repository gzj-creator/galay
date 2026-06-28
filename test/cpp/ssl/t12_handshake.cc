/**
 * @file t13_handshake.cc
 * @brief 用途：锁定同一 IO scheduler 下并发 TLS handshake 必须全部恢复完成。
 * 关键覆盖点：`SslSocket::handshake()` 在多连接并发场景下的推进与收口。
 * 通过条件：服务端和客户端 handshake 全部完成且无超时。
 */

#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-kernel/common/defn.hpp>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

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

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

constexpr uint16_t kPort = 19449;
constexpr int kConnections = 16;

struct TestState {
    std::atomic<bool> server_ready{false};
    std::atomic<int> accepted{0};
    std::atomic<int> connected{0};
    std::atomic<int> server_handshake_done{0};
    std::atomic<int> client_handshake_done{0};
    std::atomic<int> server_done{0};
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

Task<void> handleAcceptedClient(SslContext* ctx, GHandle handle, TestState* state)
{
    SslSocket client(ctx, handle);
    client.option().handleNonBlock();

    auto handshake = co_await client.handshake();
    if (!handshake) {
        fail(state, "server handshake failed");
    } else {
        state->server_handshake_done.fetch_add(1, std::memory_order_relaxed);
    }

    (void)co_await client.close();
    state->server_done.fetch_add(1, std::memory_order_relaxed);
}

Task<void> runServer(IOScheduler* scheduler, SslContext* ctx, TestState* state)
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
        if (!scheduleTask(scheduler, handleAcceptedClient(ctx, accepted.value(), state))) {
            fail(state, "schedule accepted client failed");
            break;
        }
    }

    (void)co_await listener.close();
}

Task<void> runClient(SslContext* ctx, TestState* state)
{
    SslSocket socket(ctx);
    socket.option().handleNonBlock();
    if (!socket.setHostname("localhost")) {
        fail(state, "set hostname failed");
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    auto connected = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connected) {
        fail(state, "connect failed");
        (void)co_await socket.close();
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    if ((static_cast<uint32_t>(socket.controller()->m_type) & CONNECT) != 0) {
        fail(state, "connect flag still registered after await_resume");
        (void)co_await socket.close();
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    if (socket.controller()->m_awaitable[IOController::WRITE] != nullptr) {
        fail(state, "write slot still occupied after connect await_resume");
        (void)co_await socket.close();
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    state->connected.fetch_add(1, std::memory_order_relaxed);

    auto handshake = co_await socket.handshake();
    if (!handshake) {
        fail(state, "client handshake failed");
    } else {
        state->client_handshake_done.fetch_add(1, std::memory_order_relaxed);
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

void waitForCompletion(TestState& state)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state.client_done.load(std::memory_order_relaxed) < kConnections ||
           state.server_done.load(std::memory_order_relaxed) < kConnections) {
        if (state.failed.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state.failure_mu);
            throw std::runtime_error(state.failure.empty() ? "ssl handshake fanout failed" : state.failure);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "ssl handshake fanout timed out [accepted=" +
                std::to_string(state.accepted.load(std::memory_order_relaxed)) +
                ", connected=" + std::to_string(state.connected.load(std::memory_order_relaxed)) +
                ", client_hs=" + std::to_string(state.client_handshake_done.load(std::memory_order_relaxed)) +
                ", server_hs=" + std::to_string(state.server_handshake_done.load(std::memory_order_relaxed)) +
                ", client_done=" + std::to_string(state.client_done.load(std::memory_order_relaxed)) +
                ", server_done=" + std::to_string(state.server_done.load(std::memory_order_relaxed)) + "]"
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
    expect(server_ctx.loadCertificate("certs/server.crt").has_value(), "load server cert failed");
    expect(server_ctx.loadPrivateKey("certs/server.key").has_value(), "load server key failed");
    expect(client_ctx.loadCACertificate("certs/ca.crt").has_value(), "load CA failed");
    server_ctx.setSessionCacheMode(SSL_SESS_CACHE_OFF);
    server_ctx.setSessionTimeout(0);
    client_ctx.setVerifyMode(SslVerifyMode::Peer);
    client_ctx.setSessionCacheMode(SSL_SESS_CACHE_OFF);
    client_ctx.setSessionTimeout(0);
    SSL_CTX_set_options(client_ctx.native(), SSL_OP_NO_TICKET);

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    expect(scheduleTask(scheduler, runServer(&scheduler, &server_ctx, &state)), "schedule server failed");
    waitFor(state.server_ready, "server did not become ready");

    for (int i = 0; i < kConnections; ++i) {
        expect(scheduleTask(scheduler, runClient(&client_ctx, &state)), "schedule client failed");
    }

    int rc = 0;
    try {
        waitForCompletion(state);
    } catch (const std::exception& ex) {
        std::cerr << "[T13] " << ex.what() << "\n";
        if (auto error = scheduler.lastError(); error.has_value()) {
            std::cerr << "[T13] scheduler lastError: " << error->message() << "\n";
        }
        rc = 1;
    }

    if (rc != 0) {
        std::cerr.flush();
        std::_Exit(rc);
    }

    scheduler.stop();
    if (state.client_handshake_done.load(std::memory_order_relaxed) != kConnections ||
        state.server_handshake_done.load(std::memory_order_relaxed) != kConnections) {
        std::cerr << "[T13] handshake count mismatch [client_hs="
                  << state.client_handshake_done.load(std::memory_order_relaxed)
                  << ", server_hs=" << state.server_handshake_done.load(std::memory_order_relaxed) << "]\n";
        return 1;
    }

    std::cout << "t13_handshake PASS\n";
    return 0;
}
