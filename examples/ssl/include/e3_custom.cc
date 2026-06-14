/**
 * @file e3_custom.cc
 * @brief 用途：演示如何用自定义 SSL state machine 构建一个最小 Awaitable。
 * 通过条件：本地 loopback 下服务端 custom machine 完成 `handshake -> recv -> send -> shutdown`，客户端收回 `pong`。
 */

#include "ssl/async/ssl_socket.h"
#include "ssl/ssl/ssl_context.h"
#include <kernel/kernel/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#ifdef USE_KQUEUE
#include <kernel/kernel/kqueue_scheduler.h>
using ExampleScheduler = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <kernel/kernel/epoll_scheduler.h>
using ExampleScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <kernel/kernel/io_uring_scheduler.h>
using ExampleScheduler = galay::kernel::IOUringScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

constexpr uint16_t kPort = 19543;
constexpr std::string_view kPayload = "ping";
constexpr std::string_view kReply = "pong";
using ExampleResult = std::expected<std::string, SslError>;

struct EchoMachine {
    using result_type = ExampleResult;

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
        if (!result || result.value().toStringView() != kPayload) {
            m_result = std::unexpected(result ? SslError(SslErrorCode::kReadFailed) : result.error());
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
};

struct ExampleState {
    std::atomic<bool> server_ready{false};
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
};

Task<void> serverTask(SslContext* ctx, ExampleState* state)
{
    SslSocket listener(ctx);
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    if (!listener.isValid() ||
        !listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort)) ||
        !listener.listen(16)) {
        state->done.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->server_ready.store(true, std::memory_order_relaxed);

    Host client_host;
    auto accept_result = co_await listener.accept(&client_host);
    if (!accept_result) {
        co_await listener.close();
        state->done.store(true, std::memory_order_relaxed);
        co_return;
    }

    SslSocket client(ctx, accept_result.value());
    client.option().handleNonBlock();

    auto awaitable = SslAwaitableBuilder<ExampleResult>::fromStateMachine(
        client.controller(),
        &client,
        EchoMachine{}
    ).build();

    auto result = co_await awaitable;
    state->ok.store(result.has_value() && result.value() == kReply, std::memory_order_relaxed);

    co_await client.close();
    co_await listener.close();
    state->done.store(true, std::memory_order_relaxed);
}

Task<void> clientTask(SslContext* ctx, ExampleState* state)
{
    SslSocket socket(ctx);
    socket.option().handleNonBlock();
    socket.setHostname("localhost");

    if (!socket.isValid()) {
        co_return;
    }

    auto connect_result = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connect_result) {
        co_await socket.close();
        co_return;
    }

    auto handshake_result = co_await socket.handshake();
    if (!handshake_result) {
        co_await socket.close();
        co_return;
    }

    auto send_result = co_await socket.send(kPayload.data(), kPayload.size());
    if (!send_result) {
        co_await socket.close();
        co_return;
    }

    char buffer[16];
    auto recv_result = co_await socket.recv(buffer, sizeof(buffer));
    if (recv_result && recv_result.value().toStringView() == kReply) {
        state->ok.store(true, std::memory_order_relaxed);
    }

    co_await socket.shutdown();
    co_await socket.close();
}

void waitFor(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main()
{
    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    if (!server_ctx.isValid() || !client_ctx.isValid()) {
        return 1;
    }

    if (!server_ctx.loadCertificate("certs/server.crt") ||
        !server_ctx.loadPrivateKey("certs/server.key") ||
        !client_ctx.loadCACertificate("certs/ca.crt")) {
        return 1;
    }
    client_ctx.setVerifyMode(SslVerifyMode::Peer);

    ExampleScheduler scheduler;
    scheduler.start();

    ExampleState state;
    scheduleTask(scheduler, serverTask(&server_ctx, &state));
    waitFor(state.server_ready);
    scheduleTask(scheduler, clientTask(&client_ctx, &state));
    waitFor(state.done);

    scheduler.stop();

    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << "ssl custom state machine example failed\n";
        return 1;
    }

    std::cout << "ssl custom state machine example passed\n";
    return 0;
}
