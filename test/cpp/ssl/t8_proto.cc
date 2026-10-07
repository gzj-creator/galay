/**
 * @file t8_proto.cc
 * @brief 用途：验证 SSL AwaitableBuilder 能完成 `handshake -> recv -> parse -> send -> shutdown -> finish` 协议闭环。
 * 关键覆盖点：`ParseStatus::kNeedMore`、自定义 Builder flow、单次语义 SSL 步骤。
 * 通过条件：服务端 builder 解析长度前缀 `ping` 帧后回写 `pong` 并正常 shutdown。
 */

#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-utils/buffer/byte_queue_view.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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
using ::galay::utils::ByteQueueView;

namespace {

constexpr uint16_t kPort = 19447;
using BuilderResult = std::expected<std::string, SslError>;

uint32_t read_big_endian32(const ByteQueueView& queue)
{
    auto header = queue.view(0, sizeof(uint32_t));
    return (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
}

std::array<char, 8> make_frame()
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
    explicit BuilderFlow(Scheduler* expected_scheduler = nullptr)
        : expected_scheduler(expected_scheduler) {}

    void on_await_context(const AwaitContext& ctx)
    {
        context_bound = true;
        scheduler_match = ctx.scheduler == expected_scheduler;
        task_valid = ctx.task.is_valid();
    }

    void on_handshake(SslBuilderOps<BuilderResult, 8>& ops, SslHandshakeContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
        }
    }

    void on_recv(SslBuilderOps<BuilderResult, 8>& ops, SslRecvContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        inbox.append(ctx.m_result.value().to_string_view());
    }

    ParseStatus on_parse(SslBuilderOps<BuilderResult, 8>& ops)
    {
        if (!inbox.has(sizeof(uint32_t))) {
            return ParseStatus::kNeedMore;
        }

        const size_t payload_size = read_big_endian32(inbox);
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

    void on_send(SslBuilderOps<BuilderResult, 8>& ops, SslSendContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        if (!parsed_ping || ctx.m_result.value() != reply.size()) {
            ops.complete(std::unexpected(SslError(SslErrorCode::kWriteFailed)));
            return;
        }
        sent_reply = true;
    }

    void on_shutdown(SslBuilderOps<BuilderResult, 8>& ops, SslShutdownContext& ctx)
    {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        shutdown_ok = true;
    }

    void on_finish(SslBuilderOps<BuilderResult, 8>& ops)
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
    Scheduler* expected_scheduler = nullptr;
    bool context_bound = false;
    bool scheduler_match = false;
    bool task_valid = false;
};

struct TestState {
    std::atomic<bool> serverReady{false};
    std::atomic<bool> serverDone{false};
    std::atomic<bool> clientDone{false};
    std::atomic<bool> failed{false};
    std::string builder_value;
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

Task<void> run_server(IOScheduler* scheduler, SslContext* ctx, TestState* state)
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
    client.option().handle_non_block();

    BuilderFlow flow(scheduler);
    auto awaitable = SslAwaitableBuilder<BuilderResult, 8, BuilderFlow>(
        client.controller(),
        &client,
        flow
    )
        .handshake<&BuilderFlow::on_handshake>()
        .recv<&BuilderFlow::on_recv>(flow.scratch, sizeof(flow.scratch))
        .parse<&BuilderFlow::on_parse>()
        .send<&BuilderFlow::on_send>(flow.reply.data(), flow.reply.size())
        .shutdown<&BuilderFlow::on_shutdown>()
        .finish<&BuilderFlow::on_finish>()
        .build();

    auto builder_result = co_await awaitable;
    if (!builder_result || builder_result.value() != "pong") {
        fail(state, "builder protocol failed");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }
    if (!flow.context_bound || !flow.scheduler_match || !flow.task_valid) {
        fail(state, "builder flow await context missing");
        co_await client.close();
        co_await listener.close();
        state->serverDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->builder_value = builder_result.value();

    co_await client.close();
    co_await listener.close();
    state->serverDone.store(true, std::memory_order_relaxed);
}

Task<void> run_client(SslContext* ctx, TestState* state)
{
    SslSocket socket(ctx);
    if (!socket.is_valid()) {
        fail(state, "client socket invalid");
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    socket.option().handle_non_block();
    if (!socket.set_hostname("localhost")) {
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

    const auto frame = make_frame();
    auto send_result = co_await socket.send(frame.data(), frame.size());
    if (!send_result) {
        fail(state, "client send failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    char buffer[8];
    auto recv_result = co_await socket.recv(buffer, 4);
    if (!recv_result) {
        fail(state, "client recv failed");
        co_await socket.close();
        state->clientDone.store(true, std::memory_order_relaxed);
        co_return;
    }

    state->echoed = recv_result.value().to_string();

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
    TestState state;

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
        throw std::runtime_error(state.failure.empty() ? "builder protocol failed" : state.failure);
    }

    expect(state.builder_value == "pong", "builder result mismatch");
    expect(state.echoed == "pong", "client reply mismatch");

    std::cout << "SSL builder protocol PASSED" << std::endl;
    return 0;
}
