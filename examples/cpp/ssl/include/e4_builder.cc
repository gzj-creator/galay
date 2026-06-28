/**
 * @file e4_builder.cc
 * @brief 用途：演示如何用 SSL AwaitableBuilder 构建一个最小协议流。
 * 通过条件：服务端 builder 完成 `handshake -> recv -> parse -> send -> shutdown -> finish`，客户端收回 `pong`。
 */

#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-utils/cache/byte_queue_view.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using ExampleScheduler = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using ExampleScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using ExampleScheduler = galay::kernel::IOUringScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;
using ::galay::utils::ByteQueueView;

namespace {

constexpr uint16_t kPort = 19544;
using ExampleResult = std::expected<std::string, SslError>;

uint32_t readBigEndian32(const ByteQueueView& queue)
{
    auto header = queue.view(0, sizeof(uint32_t));
    return (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
}

std::array<char, 8> makeFrame()
{
    std::array<char, 8> frame{};
    constexpr uint32_t kLength = 4;
    frame[0] = static_cast<char>((kLength >> 24) & 0xFF);
    frame[1] = static_cast<char>((kLength >> 16) & 0xFF);
    frame[2] = static_cast<char>((kLength >> 8) & 0xFF);
    frame[3] = static_cast<char>(kLength & 0xFF);
    std::memcpy(frame.data() + 4, "ping", 4);
    return frame;
}

struct BuilderFlow {
    void onHandshake(SslBuilderOps<ExampleResult, 8>& ops, SslHandshakeContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
        }
    }

    void onRecv(SslBuilderOps<ExampleResult, 8>& ops, SslRecvContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        inbox.append(ctx.m_result.value().toStringView());
    }

    ParseStatus onParse(SslBuilderOps<ExampleResult, 8>& ops)
    {
        if (!inbox.has(sizeof(uint32_t))) {
            return ParseStatus::kNeedMore;
        }
        const size_t payload_size = readBigEndian32(inbox);
        if (!inbox.has(sizeof(uint32_t) + payload_size)) {
            return ParseStatus::kNeedMore;
        }

        const std::string payload(inbox.view(sizeof(uint32_t), payload_size));
        inbox.consume(sizeof(uint32_t) + payload_size);
        if (payload != "ping") {
            ops.complete(std::unexpected(SslError(SslErrorCode::kReadFailed)));
            return ParseStatus::kCompleted;
        }

        parsed_ping = true;
        return ParseStatus::kCompleted;
    }

    void onSend(SslBuilderOps<ExampleResult, 8>& ops, SslSendContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        if (ctx.m_result.value() != reply.size()) {
            ops.complete(std::unexpected(SslError(SslErrorCode::kWriteFailed)));
            return;
        }
        sent_reply = true;
    }

    void onShutdown(SslBuilderOps<ExampleResult, 8>& ops, SslShutdownContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        shutdown_ok = true;
    }

    void onFinish(SslBuilderOps<ExampleResult, 8>& ops)
    {
        if (!parsed_ping || !sent_reply || !shutdown_ok) {
            ops.complete(std::unexpected(SslError(SslErrorCode::kUnknown)));
            return;
        }
        ops.complete(std::string(reply.data(), reply.size()));
    }

    ByteQueueView inbox;
    char scratch[6]{};
    std::array<char, 4> reply{'p', 'o', 'n', 'g'};
    bool parsed_ping = false;
    bool sent_reply = false;
    bool shutdown_ok = false;
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

    BuilderFlow flow;
    auto awaitable = SslAwaitableBuilder<ExampleResult, 8, BuilderFlow>(
        client.controller(),
        &client,
        flow
    )
        .handshake<&BuilderFlow::onHandshake>()
        .recv<&BuilderFlow::onRecv>(flow.scratch, sizeof(flow.scratch))
        .parse<&BuilderFlow::onParse>()
        .send<&BuilderFlow::onSend>(flow.reply.data(), flow.reply.size())
        .shutdown<&BuilderFlow::onShutdown>()
        .finish<&BuilderFlow::onFinish>()
        .build();

    auto result = co_await awaitable;
    state->ok.store(result.has_value() && result.value() == "pong", std::memory_order_relaxed);

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

    const auto frame = makeFrame();
    auto send_result = co_await socket.send(frame.data(), frame.size());
    if (!send_result) {
        co_await socket.close();
        co_return;
    }

    char buffer[8];
    auto recv_result = co_await socket.recv(buffer, 4);
    if (recv_result && recv_result.value().toStringView() == "pong") {
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
        std::cerr << "ssl builder protocol example failed\n";
        return 1;
    }

    std::cout << "ssl builder protocol example passed\n";
    return 0;
}
