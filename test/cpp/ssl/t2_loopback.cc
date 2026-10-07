#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

constexpr uint16_t kPort = 19443;
constexpr std::string_view kPayload = "ping-from-test";

struct SmokeState {
    std::atomic<bool> serverReady{false};
    std::atomic<bool> serverDone{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> failed{false};
    std::string echoed;
    std::string failure;
};

void fail(SmokeState* state, std::string message)
{
    state->failed.store(true, std::memory_order_relaxed);
    if (state->failure.empty()) {
        state->failure = std::move(message);
    }
}

Task<void> run_server(IOScheduler* scheduler, SslContext* ctx, SmokeState* state)
{
    (void)scheduler;
    SslSocket listener(ctx);
    if (!listener.is_valid()) {
        fail(state, "listener invalid");
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    listener.option().handle_reuse_addr();
    listener.option().handle_non_block();

    auto bindResult = listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!bindResult) {
        fail(state, "bind failed");
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto listenResult = listener.listen(16);
    if (!listenResult) {
        fail(state, "listen failed");
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->serverReady.store(true, std::memory_order_relaxed);

    Host clientHost;
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        fail(state, "accept failed");
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    SslSocket client(ctx, acceptResult.value());
    client.option().handle_non_block();

    auto handshakeResult = co_await client.handshake();
    if (!handshakeResult) {
        fail(state, "server handshake failed");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    char buffer[1024];
    auto recvResult = co_await client.recv(buffer, sizeof(buffer));
    if (!recvResult) {
        fail(state, "server recv failed");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto& bytes = recvResult.value();
    if (bytes.to_string_view() != kPayload) {
        fail(state, "server payload mismatch");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto sendResult = co_await client.send(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!sendResult) {
        fail(state, "server send failed");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto shutdownResult = co_await client.shutdown();
    if (!shutdownResult) {
        fail(state, "server shutdown failed");
    }

    co_await client.close();
    co_await listener.close();
    state->serverDone.store(true, std::memory_order_relaxed);
}

Task<void> run_client(SslContext* ctx, SmokeState* state)
{
    SslSocket socket(ctx);
    if (!socket.is_valid()) {
        fail(state, "client socket invalid");
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    socket.option().handle_non_block();
    auto hostnameResult = socket.set_hostname("localhost");
    if (!hostnameResult) {
        fail(state, "set hostname failed");
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto connectResult = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connectResult) {
        fail(state, "connect failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto handshakeResult = co_await socket.handshake();
    if (!handshakeResult) {
        fail(state, "client handshake failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto sendResult = co_await socket.send(kPayload.data(), kPayload.size());
    if (!sendResult) {
        fail(state, "client send failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    char buffer[1024];
    auto recvResult = co_await socket.recv(buffer, sizeof(buffer));
    if (!recvResult) {
        fail(state, "client recv failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->echoed = recvResult.value().to_string();

    auto shutdownResult = co_await socket.shutdown();
    if (!shutdownResult) {
        fail(state, "client shutdown failed");
    }

    co_await socket.close();
    state->clientDone.store(true, std::memory_order_relaxed);
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void wait_for(std::atomic<bool>& flag, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main()
{
    SmokeState state;

    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    expect(server_ctx.is_valid(), "server context invalid");
    expect(client_ctx.is_valid(), "client context invalid");

    expect(server_ctx.load_certificate("certs/server.crt").has_value(), "load server cert failed");
    expect(server_ctx.load_private_key("certs/server.key").has_value(), "load server key failed");
    expect(client_ctx.load_ca_certificate("certs/ca.crt").has_value(), "load CA failed");
    client_ctx.set_verify_mode(SslVerifyMode::Peer);

    TestScheduler scheduler;
    scheduler.start();

    expect(schedule_task(scheduler, run_server(&scheduler, &server_ctx, &state)), "spawn server failed");
    wait_for(state.serverReady, "server did not become ready");
    expect(schedule_task(scheduler, run_client(&client_ctx, &state)), "spawn client failed");

    wait_for(state.clientDone, "client did not finish");
    wait_for(state.serverDone, "server did not finish");

    scheduler.stop();

    if (state.failed.load(std::memory_order_relaxed)) {
        throw std::runtime_error(state.failure.empty() ? "loopback smoke failed" : state.failure);
    }

    expect(state.echoed == kPayload, "echoed payload mismatch");

    std::cout << "Loopback smoke PASSED" << std::endl;
    return 0;
}
