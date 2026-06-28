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

Task<void> runServer(IOScheduler* scheduler, SslContext* ctx, SmokeState* state)
{
    (void)scheduler;
    SslSocket listener(ctx);
    if (!listener.isValid()) {
        fail(state, "listener invalid");
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

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
    client.option().handleNonBlock();

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
    if (bytes.toStringView() != kPayload) {
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

Task<void> runClient(SslContext* ctx, SmokeState* state)
{
    SslSocket socket(ctx);
    if (!socket.isValid()) {
        fail(state, "client socket invalid");
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    socket.option().handleNonBlock();
    auto hostnameResult = socket.setHostname("localhost");
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

    state->echoed = recvResult.value().toString();

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

void waitFor(std::atomic<bool>& flag, const char* message)
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

    SslContext serverCtx(SslMethod::TLS_Server);
    SslContext clientCtx(SslMethod::TLS_Client);
    expect(serverCtx.isValid(), "server context invalid");
    expect(clientCtx.isValid(), "client context invalid");

    expect(serverCtx.loadCertificate("certs/server.crt").has_value(), "load server cert failed");
    expect(serverCtx.loadPrivateKey("certs/server.key").has_value(), "load server key failed");
    expect(clientCtx.loadCACertificate("certs/ca.crt").has_value(), "load CA failed");
    clientCtx.setVerifyMode(SslVerifyMode::Peer);

    TestScheduler scheduler;
    scheduler.start();

    expect(scheduleTask(scheduler, runServer(&scheduler, &serverCtx, &state)), "spawn server failed");
    waitFor(state.serverReady, "server did not become ready");
    expect(scheduleTask(scheduler, runClient(&clientCtx, &state)), "spawn client failed");

    waitFor(state.clientDone, "client did not finish");
    waitFor(state.serverDone, "server did not finish");

    scheduler.stop();

    if (state.failed.load(std::memory_order_relaxed)) {
        throw std::runtime_error(state.failure.empty() ? "loopback smoke failed" : state.failure);
    }

    expect(state.echoed == kPayload, "echoed payload mismatch");

    std::cout << "Loopback smoke PASSED" << std::endl;
    return 0;
}
