/**
 * @file t11_steady.cc
 * @brief 用途：验证默认 SslContext（开启默认 session cache / ticket 行为）下的 steady-state echo 不会中途断流。
 * 关键覆盖点：`SslSocket::handshake/send/recv/shutdown` 在多连接默认 TLS 上下文下的长时间运行。
 * 通过条件：16 个连接持续 echo 1024B 负载，全部完成，无 send/recv/peer-closed/mismatch。
 */

#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
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

using namespace galay::ssl;
using namespace galay::kernel;

namespace {

constexpr uint16_t kPort = 19446;
constexpr int kConnections = 16;
constexpr int kRoundsPerConn = 1000;
constexpr size_t kPayloadSize = 1024;

struct SteadyState {
    std::atomic<bool> server_ready{false};
    std::atomic<int> server_done{0};
    std::atomic<int> client_done{0};
    std::atomic<int> server_handshake_done{0};
    std::atomic<int> client_handshake_done{0};
    std::atomic<int> accepted{0};
    std::atomic<int> connected{0};
    std::atomic<int> server_recv_ops{0};
    std::atomic<int> server_send_ops{0};
    std::atomic<int> client_send_ops{0};
    std::atomic<int> client_recv_ops{0};
    std::atomic<bool> failed{false};
    std::string failure;
};

void fail(SteadyState* state, std::string message)
{
    state->failed.store(true, std::memory_order_relaxed);
    if (state->failure.empty()) {
        state->failure = std::move(message);
    }
}

std::string benchmarkCertPath(const char* name)
{
    return std::string(GALAY_SSL_BENCHMARK_CERT_DIR) + "/" + name;
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[FAIL] " << message << std::endl;
        return false;
    }
    return true;
}

bool waitFor(std::atomic<bool>& flag, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[FAIL] " << message << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool waitForCount(std::atomic<int>& count, int expected, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (count.load(std::memory_order_relaxed) < expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[FAIL] " << message << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool waitForCompletion(SteadyState& state, std::atomic<int>& count, int expected, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (count.load(std::memory_order_relaxed) < expected) {
        if (state.failed.load(std::memory_order_relaxed)) {
            std::cerr << "[FAIL] " << (state.failure.empty() ? message : state.failure) << std::endl;
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[FAIL] " << message
                      << " [client_done=" << state.client_done.load(std::memory_order_relaxed)
                      << ", server_done=" << state.server_done.load(std::memory_order_relaxed)
                      << ", accepted=" << state.accepted.load(std::memory_order_relaxed)
                      << ", connected=" << state.connected.load(std::memory_order_relaxed)
                      << ", client_hs=" << state.client_handshake_done.load(std::memory_order_relaxed)
                      << ", server_hs=" << state.server_handshake_done.load(std::memory_order_relaxed)
                      << ", client_send=" << state.client_send_ops.load(std::memory_order_relaxed)
                      << ", client_recv=" << state.client_recv_ops.load(std::memory_order_relaxed)
                      << ", server_recv=" << state.server_recv_ops.load(std::memory_order_relaxed)
                      << ", server_send=" << state.server_send_ops.load(std::memory_order_relaxed)
                      << "]" << std::endl;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

Task<void> handleAcceptedClient(SslContext* ctx, GHandle handle, SteadyState* state)
{
    SslSocket client(ctx, handle);
    client.option().handleNonBlock();

    auto handshake_result = co_await client.handshake();
    if (!handshake_result) {
        fail(state, "server handshake failed");
        co_await client.close();
        state->server_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    state->server_handshake_done.fetch_add(1, std::memory_order_relaxed);

    std::vector<char> recv_buffer(kPayloadSize);
    for (int round = 0; round < kRoundsPerConn; ++round) {
        auto recv_result = co_await client.recv(recv_buffer.data(), recv_buffer.size());
        if (!recv_result) {
            fail(state, "server recv failed");
            break;
        }
        state->server_recv_ops.fetch_add(1, std::memory_order_relaxed);
        if (recv_result->size() != kPayloadSize) {
            fail(state, "server recv size mismatch");
            break;
        }

        auto send_result = co_await client.send(recv_buffer.data(), recv_result->size());
        if (!send_result || send_result.value() != recv_result->size()) {
            fail(state, "server send failed");
            break;
        }
        state->server_send_ops.fetch_add(1, std::memory_order_relaxed);
    }

    (void)co_await client.shutdown();
    (void)co_await client.close();
    state->server_done.fetch_add(1, std::memory_order_relaxed);
}

Task<void> runServer(IOScheduler* scheduler, SslContext* ctx, SteadyState* state)
{
    SslSocket listener(ctx);
    if (!listener.isValid()) {
        fail(state, "listener invalid");
        co_return;
    }

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

    state->server_ready.store(true, std::memory_order_relaxed);

    for (int accepted = 0; accepted < kConnections; ++accepted) {
        Host client_host;
        auto accept_result = co_await listener.accept(&client_host);
        if (!accept_result) {
            fail(state, "accept failed");
            break;
        }
        state->accepted.fetch_add(1, std::memory_order_relaxed);
        if (!scheduleTask(scheduler, handleAcceptedClient(ctx, accept_result.value(), state))) {
            fail(state, "schedule accepted client failed");
            break;
        }
    }

    (void)co_await listener.close();
}

Task<void> runClient(SslContext* ctx, SteadyState* state, int client_id)
{
    SslSocket socket(ctx);
    if (!socket.isValid()) {
        fail(state, "client socket invalid");
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    socket.option().handleNonBlock();
    if (!socket.setHostname("localhost")) {
        fail(state, "set hostname failed");
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }

    auto connect_result = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connect_result) {
        fail(state, "connect failed");
        (void)co_await socket.close();
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    state->connected.fetch_add(1, std::memory_order_relaxed);

    auto handshake_result = co_await socket.handshake();
    if (!handshake_result) {
        fail(state, "client handshake failed");
        (void)co_await socket.close();
        state->client_done.fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    state->client_handshake_done.fetch_add(1, std::memory_order_relaxed);

    std::string payload(kPayloadSize, static_cast<char>('A' + (client_id % 23)));
    std::vector<char> recv_buffer(kPayloadSize);
    for (int round = 0; round < kRoundsPerConn; ++round) {
        auto send_result = co_await socket.send(payload.data(), payload.size());
        if (!send_result || send_result.value() != payload.size()) {
            fail(state, "client send failed");
            break;
        }
        state->client_send_ops.fetch_add(1, std::memory_order_relaxed);

        auto recv_result = co_await socket.recv(recv_buffer.data(), recv_buffer.size());
        if (!recv_result) {
            fail(state, "client recv failed");
            break;
        }
        state->client_recv_ops.fetch_add(1, std::memory_order_relaxed);
        if (recv_result->size() != payload.size()) {
            fail(state, "client recv size mismatch");
            break;
        }
        if (std::memcmp(recv_buffer.data(), payload.data(), payload.size()) != 0) {
            fail(state, "client payload mismatch");
            break;
        }
    }

    (void)co_await socket.shutdown();
    (void)co_await socket.close();
    state->client_done.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

int main()
{
    SteadyState state;

    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    if (!expect(server_ctx.isValid(), "server context invalid") ||
        !expect(client_ctx.isValid(), "client context invalid")) {
        return 2;
    }

    if (!expect(server_ctx.setCiphersuites("TLS_AES_128_GCM_SHA256").has_value(),
                "set server TLS 1.3 ciphersuite failed") ||
        !expect(client_ctx.setCiphersuites("TLS_AES_128_GCM_SHA256").has_value(),
                "set client TLS 1.3 ciphersuite failed")) {
        return 2;
    }

    if (!expect(server_ctx.loadCertificate(benchmarkCertPath("server.crt")).has_value(), "load server cert failed") ||
        !expect(server_ctx.loadPrivateKey(benchmarkCertPath("server.key")).has_value(), "load server key failed") ||
        !expect(client_ctx.loadCACertificate(benchmarkCertPath("ca.crt")).has_value(), "load CA failed")) {
        std::cerr << "[SKIP] missing TLS benchmark certificates under "
                  << GALAY_SSL_BENCHMARK_CERT_DIR << std::endl;
        return 0;
    }
    client_ctx.setVerifyMode(SslVerifyMode::Peer);

    TestScheduler scheduler;
    scheduler.start();

    int exit_code = 0;
    const auto benchmark_start = std::chrono::steady_clock::now();
    if (!expect(scheduleTask(scheduler, runServer(&scheduler, &server_ctx, &state)), "spawn server failed") ||
        !waitFor(state.server_ready, "server did not become ready")) {
        exit_code = 3;
    }

    for (int i = 0; exit_code == 0 && i < kConnections; ++i) {
        if (!expect(scheduleTask(scheduler, runClient(&client_ctx, &state, i)), "spawn client failed")) {
            exit_code = 4;
        }
    }

    if (exit_code == 0 &&
        (!waitForCompletion(state, state.client_done, kConnections, "clients did not finish") ||
         !waitForCompletion(state, state.server_done, kConnections, "server handlers did not finish"))) {
        exit_code = 5;
    }

    scheduler.stop();
    const auto benchmark_end = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        benchmark_end - benchmark_start
    ).count();

    const auto client_send_ops = state.client_send_ops.load(std::memory_order_relaxed);
    const auto client_recv_ops = state.client_recv_ops.load(std::memory_order_relaxed);
    const auto server_recv_ops = state.server_recv_ops.load(std::memory_order_relaxed);
    const auto server_send_ops = state.server_send_ops.load(std::memory_order_relaxed);

    std::cout << "\nSSL steady-state hot-path benchmark:" << std::endl;
    std::cout << "Connections: " << kConnections
              << ", rounds/conn: " << kRoundsPerConn
              << ", payload bytes: " << kPayloadSize << std::endl;
    std::cout << "Client send/recv ops: " << client_send_ops << "/" << client_recv_ops
              << ", server recv/send ops: " << server_recv_ops << "/" << server_send_ops
              << ", elapsed: " << elapsed_ms << " ms" << std::endl;
    if (elapsed_ms > 0) {
        const auto echo_rounds = std::min(client_send_ops, client_recv_ops);
        const double rps = static_cast<double>(echo_rounds) * 1000.0 /
                           static_cast<double>(elapsed_ms);
        const double mib = static_cast<double>(echo_rounds * kPayloadSize * 2) /
                           1024.0 / 1024.0;
        const double throughput = mib * 1000.0 / static_cast<double>(elapsed_ms);
        std::cout << "Echo round-trips/sec: " << rps
                  << ", plaintext throughput: " << throughput << " MiB/s" << std::endl;
    }

    if (exit_code == 0 && state.failed.load(std::memory_order_relaxed)) {
        std::cerr << "[FAIL] "
                  << (state.failure.empty() ? "default context steady-state failed" : state.failure)
                  << std::endl;
        exit_code = 6;
    }

    return exit_code;
}
