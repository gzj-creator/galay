/**
 * @file b31_tcp_socket_fair_throughput.cc
 * @brief Galay coroutine TCP echo benchmark used by the formal Asio comparison.
 *
 * The benchmark keeps one fixed-size request in flight per long-lived client.
 * Both the request and response are framed with exact stream reads/writes so
 * packet coalescing and partial socket operations cannot change the workload.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {

constexpr std::size_t kClients = 100;
constexpr std::size_t kServerWorkers = 4;
constexpr std::size_t kPayloadBytes = 256;
constexpr std::uint16_t kServerPort = 9091;
constexpr auto kWarmup = std::chrono::seconds(1);
constexpr auto kDuration = std::chrono::seconds(5);
constexpr auto kDrain = std::chrono::milliseconds(250);
constexpr auto kIoTimeout = std::chrono::milliseconds(50);
constexpr auto kAcceptTimeout = std::chrono::milliseconds(100);
constexpr char kWarmupMarker = 'W';
constexpr char kMeasuredMarker = 'M';

enum class Phase : std::uint8_t {
    warmup,
    measured,
    drain,
    stopped,
};

struct StatsSnapshot {
    std::uint64_t client_sent = 0;
    std::uint64_t client_received = 0;
    std::uint64_t client_bytes_sent = 0;
    std::uint64_t client_bytes_received = 0;
    std::uint64_t server_received = 0;
    std::uint64_t server_sent = 0;
    std::uint64_t server_bytes_received = 0;
    std::uint64_t server_bytes_sent = 0;
    std::uint64_t runtime_errors = 0;
    std::uint64_t shutdown_errors = 0;
};

struct ExactResult {
    bool complete = false;
    bool timed_out = false;
    bool eof = false;
    bool failed = false;
    std::uint64_t error_code = 0;
};

std::atomic<Phase> g_phase{Phase::stopped};
std::atomic<std::uint64_t> g_client_sent{0};
std::atomic<std::uint64_t> g_client_received{0};
std::atomic<std::uint64_t> g_client_bytes_sent{0};
std::atomic<std::uint64_t> g_client_bytes_received{0};
std::atomic<std::uint64_t> g_server_received{0};
std::atomic<std::uint64_t> g_server_sent{0};
std::atomic<std::uint64_t> g_server_bytes_received{0};
std::atomic<std::uint64_t> g_server_bytes_sent{0};
std::atomic<std::uint64_t> g_runtime_errors{0};
std::atomic<std::uint64_t> g_shutdown_errors{0};
std::atomic<std::uint32_t> g_client_ready{0};
std::atomic<std::uint32_t> g_client_failed{0};
std::atomic<std::uint32_t> g_server_ready{0};
std::atomic<std::uint32_t> g_server_failed{0};
std::atomic<std::uint32_t> g_server_connections_started{0};
std::atomic<std::uint32_t> g_server_connections_done{0};

galay::benchmark::CompletionLatch* g_client_completion = nullptr;
galay::benchmark::CompletionLatch* g_server_completion = nullptr;

constexpr const char* benchmarkBackend() noexcept
{
#if defined(USE_KQUEUE)
    return "kqueue";
#elif defined(USE_IOURING)
    return "io_uring";
#elif defined(USE_EPOLL)
    return "epoll";
#else
    return "unknown";
#endif
}

void addCounter(std::atomic<std::uint64_t>& counter,
                std::uint64_t value = 1) noexcept
{
    const auto previous = counter.fetch_add(value, std::memory_order_relaxed);
    if (previous > std::numeric_limits<std::uint64_t>::max() - value) {
        counter.store(std::numeric_limits<std::uint64_t>::max(),
                      std::memory_order_relaxed);
    }
}

void recordError(std::uint64_t error_code) noexcept
{
    if (IOError::contains(error_code, kTimeout)) {
        return;
    }
    if (g_phase.load(std::memory_order_acquire) >= Phase::drain) {
        addCounter(g_shutdown_errors);
    } else {
        addCounter(g_runtime_errors);
    }
}

StatsSnapshot snapshotStats() noexcept
{
    return {
        .client_sent = g_client_sent.load(std::memory_order_relaxed),
        .client_received = g_client_received.load(std::memory_order_relaxed),
        .client_bytes_sent = g_client_bytes_sent.load(std::memory_order_relaxed),
        .client_bytes_received = g_client_bytes_received.load(std::memory_order_relaxed),
        .server_received = g_server_received.load(std::memory_order_relaxed),
        .server_sent = g_server_sent.load(std::memory_order_relaxed),
        .server_bytes_received = g_server_bytes_received.load(std::memory_order_relaxed),
        .server_bytes_sent = g_server_bytes_sent.load(std::memory_order_relaxed),
        .runtime_errors = g_runtime_errors.load(std::memory_order_relaxed),
        .shutdown_errors = g_shutdown_errors.load(std::memory_order_relaxed),
    };
}

bool settledCountersMatch(const StatsSnapshot& values) noexcept
{
    return values.client_sent == values.client_received &&
           values.client_received == values.server_received &&
           values.server_received == values.server_sent &&
           values.client_bytes_sent == values.client_bytes_received &&
           values.client_bytes_received == values.server_bytes_received &&
           values.server_bytes_received == values.server_bytes_sent;
}

void resetMeasurementCounters() noexcept
{
    g_client_sent.store(0, std::memory_order_relaxed);
    g_client_received.store(0, std::memory_order_relaxed);
    g_client_bytes_sent.store(0, std::memory_order_relaxed);
    g_client_bytes_received.store(0, std::memory_order_relaxed);
    g_server_received.store(0, std::memory_order_relaxed);
    g_server_sent.store(0, std::memory_order_relaxed);
    g_server_bytes_received.store(0, std::memory_order_relaxed);
    g_server_bytes_sent.store(0, std::memory_order_relaxed);
    g_runtime_errors.store(0, std::memory_order_relaxed);
    g_shutdown_errors.store(0, std::memory_order_relaxed);
}

bool waitForCount(const std::atomic<std::uint32_t>& value,
                  std::uint32_t target,
                  std::chrono::seconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) >= target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return value.load(std::memory_order_acquire) >= target;
}

void markClientStartupFailed() noexcept
{
    g_client_failed.fetch_add(1, std::memory_order_release);
}

void markServerStartupFailed() noexcept
{
    g_server_failed.fetch_add(1, std::memory_order_release);
}

void reportStartupError(const char* operation, std::uint64_t error_code) noexcept
{
    std::cerr << "tcp_server_startup_error operation=" << operation
              << " code=" << error_code
              << " code_hex=0x" << std::hex << error_code << std::dec << '\n';
}

// TCP may split a frame across multiple recv operations.  Keep the current
// offset across timeout wakeups so the fixed request framing remains intact.
Task<ExactResult> readExact(AsyncTcpSocket& socket,
                            char* buffer,
                            std::size_t length,
                            std::size_t& offset)
{
    while (offset < length) {
        auto result = co_await socket.recv(buffer + offset, length - offset)
                                .timeout(kIoTimeout);
        if (!result) {
            if (IOError::contains(result.error().code(), kTimeout)) {
                co_return ExactResult{.timed_out = true};
            }
            co_return ExactResult{.failed = true,
                                  .error_code = result.error().code()};
        }
        if (result.value() == 0) {
            co_return ExactResult{.eof = true};
        }
        offset += result.value();
    }
    co_return ExactResult{.complete = true};
}

Task<ExactResult> writeAll(AsyncTcpSocket& socket,
                           const char* buffer,
                           std::size_t length,
                           std::size_t& offset)
{
    while (offset < length) {
        auto result = co_await socket.send(buffer + offset, length - offset)
                                .timeout(kIoTimeout);
        if (!result) {
            if (IOError::contains(result.error().code(), kTimeout)) {
                co_return ExactResult{.timed_out = true};
            }
            co_return ExactResult{.failed = true,
                                  .error_code = result.error().code()};
        }
        if (result.value() == 0) {
            co_return ExactResult{.eof = true};
        }
        offset += result.value();
    }
    co_return ExactResult{.complete = true};
}

Task<void> tcpServerConnection(AsyncTcpSocket client)
{
    if (!client.option().handleNonBlock()) {
        addCounter(g_runtime_errors);
        ++g_server_connections_done;
        co_return;
    }

    std::array<char, kPayloadBytes> buffer{};
    std::size_t read_offset = 0;
    while (g_phase.load(std::memory_order_acquire) != Phase::stopped) {
        auto read_result = co_await readExact(client, buffer.data(), buffer.size(), read_offset);
        if (!read_result) {
            addCounter(g_runtime_errors);
            break;
        }
        const ExactResult read = *read_result;
        if (read.timed_out) {
            continue;
        }
        if (!read.complete) {
            if (read.failed) {
                recordError(read.error_code);
            }
            break;
        }
        read_offset = 0;

        const bool measured_frame = buffer[0] == kMeasuredMarker;
        if (measured_frame) {
            addCounter(g_server_received);
            addCounter(g_server_bytes_received, buffer.size());
        }

        std::size_t write_offset = 0;
        while (write_offset < buffer.size() &&
               g_phase.load(std::memory_order_acquire) != Phase::stopped) {
            auto write_result = co_await writeAll(
                client, buffer.data(), buffer.size(), write_offset);
            if (!write_result) {
                addCounter(g_runtime_errors);
                break;
            }
            const ExactResult write = *write_result;
            if (write.timed_out) {
                continue;
            }
            if (!write.complete) {
                if (write.failed) {
                    recordError(write.error_code);
                }
                write_offset = buffer.size();
                break;
            }
        }
        if (write_offset != buffer.size()) {
            break;
        }
        if (measured_frame) {
            addCounter(g_server_sent);
            addCounter(g_server_bytes_sent, buffer.size());
        }
    }

    const auto closed = co_await client.close();
    if (!closed) {
        recordError(closed.error().code());
    }
    g_server_connections_done.fetch_add(1, std::memory_order_release);
    co_return;
}

Task<void> tcpServerWorker(Scheduler* scheduler, int worker_id)
{
    auto socket_result = AsyncTcpSocket::create(IPType::IPV4);
    if (!socket_result) {
        reportStartupError("create", socket_result.error().code());
        recordError(socket_result.error().code());
        markServerStartupFailed();
        if (g_server_completion) g_server_completion->arrive();
        co_return;
    }
    AsyncTcpSocket listener = std::move(*socket_result);

    const auto reuse_addr = listener.option().handleReuseAddr();
    const auto reuse_port = listener.option().handleReusePort();
    const auto non_block = listener.option().handleNonBlock();
    if (!reuse_addr || !reuse_port || !non_block) {
        if (!reuse_addr) {
            reportStartupError("reuse_addr", reuse_addr.error().code());
            recordError(reuse_addr.error().code());
        }
        if (!reuse_port) {
            reportStartupError("reuse_port", reuse_port.error().code());
            recordError(reuse_port.error().code());
        }
        if (!non_block) {
            reportStartupError("non_block", non_block.error().code());
            recordError(non_block.error().code());
        }
        markServerStartupFailed();
        if (g_server_completion) g_server_completion->arrive();
        co_return;
    }

    const Host endpoint(IPType::IPV4, "127.0.0.1", kServerPort);
    const auto bound = listener.bind(endpoint);
    if (!bound) {
        reportStartupError("bind", bound.error().code());
        recordError(bound.error().code());
        markServerStartupFailed();
        if (g_server_completion) g_server_completion->arrive();
        co_return;
    }
    const auto listening = listener.listen(1024);
    if (!listening) {
        reportStartupError("listen", listening.error().code());
        recordError(listening.error().code());
        markServerStartupFailed();
        if (g_server_completion) g_server_completion->arrive();
        co_return;
    }

    g_server_ready.fetch_add(1, std::memory_order_release);
    if (worker_id == 0) {
        LogInfo("TCP server workers started on 127.0.0.1:{}", kServerPort);
    }

    while (g_phase.load(std::memory_order_acquire) != Phase::stopped) {
        Host peer;
        auto accepted = co_await listener.accept(&peer).timeout(kAcceptTimeout);
        if (!accepted) {
            if (IOError::contains(accepted.error().code(), kTimeout)) {
                continue;
            }
            if (g_phase.load(std::memory_order_acquire) != Phase::stopped) {
                recordError(accepted.error().code());
            }
            break;
        }

        AsyncTcpSocket client(accepted.value());
        const auto client_non_block = client.option().handleNonBlock();
        const auto no_delay = client.option().handleTcpNoDelay();
        if (!client_non_block || !no_delay) {
            if (!client_non_block) recordError(client_non_block.error().code());
            if (!no_delay) recordError(no_delay.error().code());
            const auto closed = co_await client.close();
            if (!closed) recordError(closed.error().code());
            continue;
        }

        g_server_connections_started.fetch_add(1, std::memory_order_release);
        if (!scheduleTask(*scheduler, tcpServerConnection(std::move(client)))) {
            addCounter(g_runtime_errors);
            g_server_connections_done.fetch_add(1, std::memory_order_release);
        }
    }

    const auto closed = co_await listener.close();
    if (!closed) recordError(closed.error().code());
    if (g_server_completion) g_server_completion->arrive();
    co_return;
}

Task<void> tcpBenchmarkClient(int client_id)
{
    auto socket_result = AsyncTcpSocket::create(IPType::IPV4);
    if (!socket_result) {
        recordError(socket_result.error().code());
        markClientStartupFailed();
        if (g_client_completion) g_client_completion->arrive();
        co_return;
    }
    AsyncTcpSocket client = std::move(*socket_result);
    const auto non_block = client.option().handleNonBlock();
    const auto no_delay = client.option().handleTcpNoDelay();
    if (!non_block || !no_delay) {
        if (!non_block) recordError(non_block.error().code());
        if (!no_delay) recordError(no_delay.error().code());
        markClientStartupFailed();
        const auto closed = co_await client.close();
        if (!closed) recordError(closed.error().code());
        if (g_client_completion) g_client_completion->arrive();
        co_return;
    }

    const Host endpoint(IPType::IPV4, "127.0.0.1", kServerPort);
    auto connected = co_await client.connect(endpoint).timeout(std::chrono::seconds(2));
    if (!connected) {
        recordError(connected.error().code());
        markClientStartupFailed();
        const auto closed = co_await client.close();
        if (!closed) recordError(closed.error().code());
        if (g_client_completion) g_client_completion->arrive();
        co_return;
    }

    std::array<char, kPayloadBytes> payload{};
    std::array<char, kPayloadBytes> response{};
    std::fill(payload.begin(), payload.end(), static_cast<char>('a' + client_id % 26));
    g_client_ready.fetch_add(1, std::memory_order_release);
    std::size_t measured_sent = 0;
    std::size_t measured_received = 0;

    while (g_phase.load(std::memory_order_acquire) != Phase::stopped) {
        const Phase phase = g_phase.load(std::memory_order_acquire);
        if (phase == Phase::drain) {
            while (measured_received < measured_sent &&
                   g_phase.load(std::memory_order_acquire) != Phase::stopped) {
                std::size_t receive_offset = 0;
                auto receive_result = co_await readExact(
                    client, response.data(), response.size(), receive_offset);
                if (!receive_result) {
                    addCounter(g_runtime_errors);
                    break;
                }
                const ExactResult receive = *receive_result;
                if (receive.timed_out) continue;
                if (!receive.complete) {
                    if (receive.failed) recordError(receive.error_code);
                    break;
                }
                if (response[0] == kMeasuredMarker) {
                    ++measured_received;
                    addCounter(g_client_received);
                    addCounter(g_client_bytes_received, response.size());
                }
            }
            break;
        }
        const bool measured_frame = phase == Phase::measured;
        payload[0] = measured_frame ? kMeasuredMarker : kWarmupMarker;

        std::size_t send_offset = 0;
        while (send_offset < payload.size() &&
               g_phase.load(std::memory_order_acquire) != Phase::stopped) {
            auto send_result = co_await writeAll(
                client, payload.data(), payload.size(), send_offset);
            if (!send_result) {
                addCounter(g_runtime_errors);
                break;
            }
            const ExactResult send = *send_result;
            if (send.timed_out) continue;
            if (!send.complete) {
                if (send.failed) recordError(send.error_code);
                send_offset = payload.size();
                break;
            }
        }
        if (send_offset != payload.size()) {
            break;
        }
        if (measured_frame) {
            ++measured_sent;
            addCounter(g_client_sent);
            addCounter(g_client_bytes_sent, payload.size());
        }

        std::size_t receive_offset = 0;
        while (receive_offset < response.size() &&
               g_phase.load(std::memory_order_acquire) != Phase::stopped) {
            auto receive_result = co_await readExact(
                client, response.data(), response.size(), receive_offset);
            if (!receive_result) {
                addCounter(g_runtime_errors);
                break;
            }
            const ExactResult receive = *receive_result;
            if (receive.timed_out) continue;
            if (!receive.complete) {
                if (receive.failed) recordError(receive.error_code);
                receive_offset = response.size();
                break;
            }
        }
        if (receive_offset != response.size()) {
            break;
        }
        if (measured_frame && response[0] == kMeasuredMarker) {
            ++measured_received;
            addCounter(g_client_received);
            addCounter(g_client_bytes_received, response.size());
        }
    }

    const auto closed = co_await client.close();
    if (!closed) recordError(closed.error().code());
    if (g_client_completion) g_client_completion->arrive();
    co_return;
}

void printBenchmarkResults(std::chrono::steady_clock::time_point started,
                           std::chrono::steady_clock::time_point ended,
                           const StatsSnapshot& measured,
                           const StatsSnapshot& settled,
                           bool status_ok) {
    const auto measured_us = std::chrono::duration_cast<std::chrono::microseconds>(ended - started).count();
    const double seconds = static_cast<double>(measured_us) / 1'000'000.0;
    const double measured_loss = measured.client_sent == 0
        ? 100.0
        : std::max(0.0, (1.0 - static_cast<double>(measured.client_received) /
                               static_cast<double>(measured.client_sent)) * 100.0);
    const double settled_loss = settled.client_sent == 0
        ? 100.0
        : std::max(0.0, (1.0 - static_cast<double>(settled.client_received) /
                               static_cast<double>(settled.client_sent)) * 100.0);

    LogInfo("\n========== TCP Benchmark Results ==========");
    LogInfo("Test Duration: {:.2f} seconds", seconds);
    LogInfo("Concurrent Clients: {}", kClients);
    LogInfo("Ready Clients: {}", g_client_ready.load(std::memory_order_acquire));
    LogInfo("Server Workers: {}", kServerWorkers);
    LogInfo("Message Size: {} bytes", kPayloadBytes);
    LogInfo("Measurement window client: sent={} ({:.2f} pkt/s), received={} ({:.2f} pkt/s)",
            measured.client_sent, measured.client_sent / seconds,
            measured.client_received, measured.client_received / seconds);
    LogInfo("Measurement window server: received={} ({:.2f} pkt/s), sent={} ({:.2f} pkt/s)",
            measured.server_received, measured.server_received / seconds,
            measured.server_sent, measured.server_sent / seconds);
    LogInfo("Settled measured packets: client_sent={} client_received={} server_received={} server_sent={} loss={:.6f}%",
            settled.client_sent, settled.client_received, settled.server_received,
            settled.server_sent, settled_loss);
    LogInfo("Errors: runtime={} shutdown={}", measured.runtime_errors, measured.shutdown_errors);
    LogInfo("===========================================\n");

    std::cout << "meta implementation=galay version=current coroutine=galay::Task"
              << " scenario=tcp-echo backend=" << benchmarkBackend()
              << " clients=" << kClients
              << " workers=" << kServerWorkers
              << " payload_bytes=" << kPayloadBytes
              << " pipeline=1 warmup_s=1 duration_s=5"
              << " ready_clients=" << g_client_ready.load(std::memory_order_acquire)
              << " server_connections=" << g_server_connections_started.load(std::memory_order_acquire)
              << '\n';
    std::cout << "measured client_sent=" << measured.client_sent
              << " client_received=" << measured.client_received
              << " client_bytes_sent=" << measured.client_bytes_sent
              << " client_bytes_received=" << measured.client_bytes_received
              << " server_received=" << measured.server_received
              << " server_sent=" << measured.server_sent
              << " server_bytes_received=" << measured.server_bytes_received
              << " server_bytes_sent=" << measured.server_bytes_sent
              << " measurement_ms=" << measured_us / 1000
              << " client_pkt_s=" << measured.client_sent / seconds
              << " server_pkt_s=" << measured.server_received / seconds
              << " client_loss_pct=" << measured_loss
              << " runtime_errors=" << measured.runtime_errors
              << " shutdown_errors=" << measured.shutdown_errors << '\n';
    std::cout << "settled client_sent=" << settled.client_sent
              << " client_received=" << settled.client_received
              << " client_bytes_sent=" << settled.client_bytes_sent
              << " client_bytes_received=" << settled.client_bytes_received
              << " server_received=" << settled.server_received
              << " server_sent=" << settled.server_sent
              << " server_bytes_received=" << settled.server_bytes_received
              << " server_bytes_sent=" << settled.server_bytes_sent
              << " elapsed_ms=" << measured_us / 1000
              << " client_loss_pct=" << settled_loss
              << " settled_loss_pct=" << settled_loss
              << " runtime_errors=" << settled.runtime_errors
              << " shutdown_errors=" << settled.shutdown_errors << '\n';
    std::cout << "status=" << (status_ok ? "ok" : "fail") << '\n';
}

template <typename SchedulerType>
int runBenchmark(SchedulerType& scheduler)
{
    if (!scheduler.start()) {
        LogError("TCP benchmark scheduler failed to start");
        return 1;
    }

    g_phase.store(Phase::warmup, std::memory_order_release);
    g_client_ready.store(0, std::memory_order_relaxed);
    g_client_failed.store(0, std::memory_order_relaxed);
    g_server_ready.store(0, std::memory_order_relaxed);
    g_server_failed.store(0, std::memory_order_relaxed);
    g_server_connections_started.store(0, std::memory_order_relaxed);
    g_server_connections_done.store(0, std::memory_order_relaxed);
    resetMeasurementCounters();

    galay::benchmark::CompletionLatch client_completion(kClients);
    galay::benchmark::CompletionLatch server_completion(kServerWorkers);
    g_client_completion = &client_completion;
    g_server_completion = &server_completion;

    for (std::size_t worker = 0; worker < kServerWorkers; ++worker) {
        if (!scheduleTask(scheduler, tcpServerWorker(&scheduler, static_cast<int>(worker)))) {
            markServerStartupFailed();
            server_completion.arrive();
        }
    }

    bool setup_ok = waitForCount(g_server_ready, kServerWorkers, std::chrono::seconds(2)) &&
                    g_server_failed.load(std::memory_order_acquire) == 0;
    if (setup_ok) {
        for (std::size_t client = 0; client < kClients; ++client) {
            if (!scheduleTask(scheduler, tcpBenchmarkClient(static_cast<int>(client)))) {
                markClientStartupFailed();
                client_completion.arrive();
            }
        }
        setup_ok = waitForCount(g_client_ready, kClients, std::chrono::seconds(2)) &&
                   g_client_failed.load(std::memory_order_acquire) == 0;
    }
    if (setup_ok) {
        setup_ok = waitForCount(g_server_connections_started, kClients,
                                std::chrono::seconds(2));
    }

    if (!setup_ok) {
        std::cerr << "setup_failed"
                  << " server_ready=" << g_server_ready.load(std::memory_order_acquire)
                  << " server_failed=" << g_server_failed.load(std::memory_order_acquire)
                  << " client_ready=" << g_client_ready.load(std::memory_order_acquire)
                  << " client_failed=" << g_client_failed.load(std::memory_order_acquire)
                  << " server_connections_started="
                  << g_server_connections_started.load(std::memory_order_acquire)
                  << " server_connections_done="
                  << g_server_connections_done.load(std::memory_order_acquire)
                  << " runtime_errors=" << g_runtime_errors.load(std::memory_order_relaxed)
                  << " shutdown_errors=" << g_shutdown_errors.load(std::memory_order_relaxed)
                  << '\n';
        g_phase.store(Phase::stopped, std::memory_order_release);
        const bool clients_stopped = client_completion.waitFor(std::chrono::seconds(3));
        const bool servers_stopped = server_completion.waitFor(std::chrono::seconds(3));
        (void)clients_stopped;
        (void)servers_stopped;
        waitForCount(g_server_connections_done,
                     g_server_connections_started.load(std::memory_order_acquire),
                     std::chrono::seconds(3));
        scheduler.stop();
        g_client_completion = nullptr;
        g_server_completion = nullptr;
        return 1;
    }

    std::this_thread::sleep_for(kWarmup);
    if (g_runtime_errors.load(std::memory_order_relaxed) != 0) {
        std::cerr << "warmup_failed"
                  << " server_ready=" << g_server_ready.load(std::memory_order_acquire)
                  << " server_failed=" << g_server_failed.load(std::memory_order_acquire)
                  << " client_ready=" << g_client_ready.load(std::memory_order_acquire)
                  << " client_failed=" << g_client_failed.load(std::memory_order_acquire)
                  << " server_connections_started="
                  << g_server_connections_started.load(std::memory_order_acquire)
                  << " server_connections_done="
                  << g_server_connections_done.load(std::memory_order_acquire)
                  << " runtime_errors=" << g_runtime_errors.load(std::memory_order_relaxed)
                  << " shutdown_errors=" << g_shutdown_errors.load(std::memory_order_relaxed)
                  << '\n';
        g_phase.store(Phase::stopped, std::memory_order_release);
        const bool clients_stopped = client_completion.waitFor(std::chrono::seconds(3));
        const bool servers_stopped = server_completion.waitFor(std::chrono::seconds(3));
        (void)clients_stopped;
        (void)servers_stopped;
        waitForCount(g_server_connections_done,
                     g_server_connections_started.load(std::memory_order_acquire),
                     std::chrono::seconds(3));
        scheduler.stop();
        g_client_completion = nullptr;
        g_server_completion = nullptr;
        return 1;
    }

    resetMeasurementCounters();
    g_phase.store(Phase::measured, std::memory_order_release);
    const auto measurement_start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(kDuration);
    const auto measurement_end = std::chrono::steady_clock::now();
    const auto measured = snapshotStats();

    g_phase.store(Phase::drain, std::memory_order_release);
    std::this_thread::sleep_for(kDrain);
    g_phase.store(Phase::stopped, std::memory_order_release);
    const bool clients_done = client_completion.waitFor(std::chrono::seconds(3));
    const bool servers_done = server_completion.waitFor(std::chrono::seconds(3));
    const bool connections_done = waitForCount(
        g_server_connections_done,
        g_server_connections_started.load(std::memory_order_acquire),
        std::chrono::seconds(3));
    scheduler.stop();

    const auto settled = snapshotStats();
    const bool status_ok = clients_done && servers_done && connections_done &&
                           g_client_ready.load(std::memory_order_acquire) == kClients &&
                           g_server_ready.load(std::memory_order_acquire) == kServerWorkers &&
                           measured.client_sent > 0 && measured.runtime_errors == 0 &&
                           measured.shutdown_errors == 0 && settled.runtime_errors == 0 &&
                           settled.shutdown_errors == 0 && settledCountersMatch(settled);
    printBenchmarkResults(measurement_start, measurement_end, measured, settled, status_ok);

    g_client_completion = nullptr;
    g_server_completion = nullptr;
    return status_ok ? 0 : 1;
}

} // namespace

int main()
{
#if defined(USE_KQUEUE)
    KqueueScheduler scheduler;
#elif defined(USE_IOURING)
    IOUringScheduler scheduler;
#elif defined(USE_EPOLL)
    EpollScheduler scheduler;
#else
    std::cerr << "TCP benchmark requires kqueue, io_uring, or epoll\n";
    return 1;
#endif
    LogInfo("TCP Socket Fair Throughput Benchmark");
    return runBenchmark(scheduler);
}
