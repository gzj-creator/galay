/**
 * @file t6_custom.cc
 * @brief 用途：验证用户自定义 SSL state machine 可以独立完成 handshake -> recv -> send -> shutdown。
 * 关键覆盖点：`SslMachineAction::handshake/recv/send/shutdown` 全链路。
 * 通过条件：服务端完整 custom machine 跑通，客户端收回 `pong`。
 */

#include "ssl/async/ssl_socket.h"
#include "ssl/ssl/ssl_context.h"
#include <kernel/kernel/task.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef USE_KQUEUE
#include <kernel/kernel/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <kernel/kernel/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <kernel/kernel/io_uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

constexpr uint16_t kPort = 19446;
constexpr std::string_view kPayload = "ping";
constexpr std::string_view kReply = "pong";

using MachineResult = std::expected<std::string, SslError>;

struct AwaitContextCapture {
    Scheduler* expected_scheduler = nullptr;
    bool context_bound = false;
    bool scheduler_match = false;
    bool task_valid = false;
};

struct FullExchangeMachine {
    using result_type = MachineResult;

    explicit FullExchangeMachine(AwaitContextCapture* capture = nullptr)
        : capture(capture) {}

    void onAwaitContext(const AwaitContext& ctx)
    {
        capture->context_bound = true;
        capture->scheduler_match = ctx.scheduler == capture->expected_scheduler;
        capture->task_valid = ctx.task.isValid();
    }

    SslMachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_phase) {
        case Phase::kHandshake:
            return SslMachineAction<result_type>::handshake();
        case Phase::kRecv:
            return SslMachineAction<result_type>::recv(m_buffer.data(), m_buffer.size());
        case Phase::kSend:
            return SslMachineAction<result_type>::send(m_reply.data(), m_reply.size());
        case Phase::kShutdown:
            return SslMachineAction<result_type>::shutdown();
        case Phase::kDone:
            return SslMachineAction<result_type>::fail(SslError(SslErrorCode::kUnknown));
        }

        return SslMachineAction<result_type>::fail(SslError(SslErrorCode::kUnknown));
    }

    void onHandshake(std::expected<void, SslError> result)
    {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::kDone;
            return;
        }
        m_phase = Phase::kRecv;
    }

    void onRecv(std::expected<Bytes, SslError> result)
    {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::kDone;
            return;
        }
        if (result.value().toStringView() != kPayload) {
            m_result = std::unexpected(SslError(SslErrorCode::kReadFailed));
            m_phase = Phase::kDone;
            return;
        }
        m_phase = Phase::kSend;
    }

    void onSend(std::expected<size_t, SslError> result)
    {
        if (!result || result.value() != m_reply.size()) {
            m_result = std::unexpected(result ? SslError(SslErrorCode::kWriteFailed) : result.error());
            m_phase = Phase::kDone;
            return;
        }
        m_phase = Phase::kShutdown;
    }

    void onShutdown(std::expected<void, SslError> result)
    {
        if (!result) {
            m_result = std::unexpected(result.error());
        } else {
            m_result = std::string(m_reply.data(), m_reply.size());
        }
        m_phase = Phase::kDone;
    }

private:
    enum class Phase : uint8_t {
        kHandshake,
        kRecv,
        kSend,
        kShutdown,
        kDone,
    };

    Phase m_phase = Phase::kHandshake;
    std::array<char, 8> m_buffer{};
    std::array<char, 4> m_reply{'p', 'o', 'n', 'g'};
    std::optional<result_type> m_result;
    AwaitContextCapture* capture = nullptr;
};

struct TestState {
    std::atomic<bool> serverReady{false};
    std::atomic<bool> serverDone{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> failed{false};
    std::string machine_value;
    std::string echoed;
    std::string failure;
};

void fail(TestState* state, std::string message)
{
    state->failed.store(true, std::memory_order_relaxed);
    if (state->failure.empty()) {
        state->failure = std::move(message);
    }
}

Task<void> runServer(IOScheduler* scheduler, SslContext* ctx, TestState* state)
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

    if (!listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort))) {
        fail(state, "bind failed");
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }
    if (!listener.listen(16)) {
        fail(state, "listen failed");
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->serverReady.store(true, std::memory_order_relaxed);

    Host client_host;
    auto accept_result = co_await listener.accept(&client_host);
    if (!accept_result) {
        fail(state, "accept failed");
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    SslSocket client(ctx, accept_result.value());
    client.option().handleNonBlock();

    AwaitContextCapture capture{.expected_scheduler = scheduler};
    auto awaitable = SslAwaitableBuilder<MachineResult>::fromStateMachine(
        client.controller(),
        &client,
        FullExchangeMachine(&capture)
    ).build();

    auto machine_result = co_await awaitable;
    if (!machine_result || machine_result.value() != kReply) {
        fail(state, "full custom machine failed");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }
    if (!capture.context_bound || !capture.scheduler_match || !capture.task_valid) {
        fail(state, "custom machine await context missing");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->machine_value = machine_result.value();

    co_await client.close();
    co_await listener.close();
    state->serverDone.store(true, std::memory_order_relaxed);
}

Task<void> runClient(SslContext* ctx, TestState* state)
{
    SslSocket socket(ctx);
    if (!socket.isValid()) {
        fail(state, "client socket invalid");
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    socket.option().handleNonBlock();
    if (!socket.setHostname("localhost")) {
        fail(state, "set hostname failed");
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto connect_result = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connect_result) {
        fail(state, "connect failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto handshake_result = co_await socket.handshake();
    if (!handshake_result) {
        fail(state, "client handshake failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    auto send_result = co_await socket.send(kPayload.data(), kPayload.size());
    if (!send_result) {
        fail(state, "client send failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    char buffer[16];
    auto recv_result = co_await socket.recv(buffer, sizeof(buffer));
    if (!recv_result) {
        fail(state, "client recv failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->echoed = recv_result.value().toString();

    auto shutdown_result = co_await socket.shutdown();
    if (!shutdown_result) {
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
    TestState state;

    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    expect(server_ctx.isValid(), "server context invalid");
    expect(client_ctx.isValid(), "client context invalid");

    expect(server_ctx.loadCertificate("certs/server.crt").has_value(), "load server cert failed");
    expect(server_ctx.loadPrivateKey("certs/server.key").has_value(), "load server key failed");
    expect(client_ctx.loadCACertificate("certs/ca.crt").has_value(), "load CA failed");
    client_ctx.setVerifyMode(SslVerifyMode::Peer);

    TestScheduler scheduler;
    scheduler.start();

    expect(scheduleTask(scheduler, runServer(&scheduler, &server_ctx, &state)), "spawn server failed");
    waitFor(state.serverReady, "server did not become ready");
    expect(scheduleTask(scheduler, runClient(&client_ctx, &state)), "spawn client failed");

    waitFor(state.clientDone, "client did not finish");
    waitFor(state.serverDone, "server did not finish");

    scheduler.stop();

    if (state.failed.load(std::memory_order_relaxed)) {
        throw std::runtime_error(state.failure.empty() ? "custom state machine failed" : state.failure);
    }

    expect(state.machine_value == kReply, "machine reply mismatch");
    expect(state.echoed == kReply, "client reply mismatch");

    std::cout << "SSL custom state machine PASSED" << std::endl;
    return 0;
}
