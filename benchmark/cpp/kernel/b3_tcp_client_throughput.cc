/**
 * @file b3_tcp.cc
 * @brief 用途：作为 TCP 压测客户端，发起并发连接与请求以评估往返性能。
 * 关键覆盖点：并发建连、批量请求发送、响应接收以及吞吐与延迟统计。
 * 通过条件：客户端完成既定负载并输出统计结果，测试结束后干净退出。
 */

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
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

constexpr const char* benchmarkBackend() {
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

constexpr const char* benchmarkBuildMode() {
#ifdef NDEBUG
    return "release-like";
#else
    return "debug-like";
#endif
}

}  // namespace

std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<uint64_t> g_success_count{0};
std::atomic<uint64_t> g_error_count{0};
std::atomic<uint64_t> g_connect_attempts{0};
std::atomic<uint64_t> g_connect_success{0};
std::atomic<uint64_t> g_connect_failed{0};
std::atomic<uint64_t> g_connect_timeout{0};
std::atomic<uint64_t> g_connect_latency_total_us{0};
std::atomic<uint64_t> g_connect_latency_max_us{0};
std::atomic<bool> g_running{true};
std::mutex g_connect_samples_mu;
std::vector<uint32_t> g_connect_samples_us;
std::mutex g_connect_errors_mu;
std::unordered_map<uint64_t, uint64_t> g_connect_error_hist;
galay::benchmark::CompletionLatch* g_connected_latch = nullptr;
galay::benchmark::StartGate* g_start_gate = nullptr;

struct BenchConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    int connections = 100;
    int messageSize = 256;
    int duration = 10;  // seconds
    int connectTimeoutMs = 0;
    bool connectOnly = false;
};

struct ConnectLatencySummary {
    double avgUs = 0.0;
    uint32_t p50Us = 0;
    uint32_t p90Us = 0;
    uint32_t p99Us = 0;
    uint32_t maxUs = 0;
};

void resetBenchStats() {
    g_total_requests.store(0, std::memory_order_relaxed);
    g_total_bytes.store(0, std::memory_order_relaxed);
    g_success_count.store(0, std::memory_order_relaxed);
    g_error_count.store(0, std::memory_order_relaxed);
    g_connect_attempts.store(0, std::memory_order_relaxed);
    g_connect_success.store(0, std::memory_order_relaxed);
    g_connect_failed.store(0, std::memory_order_relaxed);
    g_connect_timeout.store(0, std::memory_order_relaxed);
    g_connect_latency_total_us.store(0, std::memory_order_relaxed);
    g_connect_latency_max_us.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_connect_samples_mu);
        g_connect_samples_us.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_connect_errors_mu);
        g_connect_error_hist.clear();
    }
}

void recordConnectLatency(uint64_t latency_us) {
    g_connect_latency_total_us.fetch_add(latency_us, std::memory_order_relaxed);
    uint64_t observed_max = g_connect_latency_max_us.load(std::memory_order_relaxed);
    while (observed_max < latency_us &&
           !g_connect_latency_max_us.compare_exchange_weak(observed_max, latency_us,
                                                           std::memory_order_relaxed)) {
    }

    std::lock_guard<std::mutex> lock(g_connect_samples_mu);
    if (latency_us > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        g_connect_samples_us.push_back(std::numeric_limits<uint32_t>::max());
    } else {
        g_connect_samples_us.push_back(static_cast<uint32_t>(latency_us));
    }
}

void recordConnectError(const IOError& error) {
    std::lock_guard<std::mutex> lock(g_connect_errors_mu);
    g_connect_error_hist[error.code()]++;
}

uint32_t percentileValue(std::vector<uint32_t>& samples, double p) {
    if (samples.empty()) {
        return 0;
    }
    if (p <= 0.0) {
        return samples.front();
    }
    if (p >= 1.0) {
        return samples.back();
    }
    const size_t index = static_cast<size_t>(p * static_cast<double>(samples.size() - 1));
    return samples[index];
}

ConnectLatencySummary summarizeConnectLatency() {
    std::vector<uint32_t> samples;
    {
        std::lock_guard<std::mutex> lock(g_connect_samples_mu);
        samples = g_connect_samples_us;
    }

    if (samples.empty()) {
        return {};
    }

    std::sort(samples.begin(), samples.end());
    ConnectLatencySummary summary;
    summary.avgUs = static_cast<double>(g_connect_latency_total_us.load(std::memory_order_relaxed)) /
                    static_cast<double>(samples.size());
    summary.p50Us = percentileValue(samples, 0.50);
    summary.p90Us = percentileValue(samples, 0.90);
    summary.p99Us = percentileValue(samples, 0.99);
    summary.maxUs = samples.back();
    return summary;
}

void printConnectErrorHistogram() {
    std::vector<std::pair<uint64_t, uint64_t>> sorted;
    {
        std::lock_guard<std::mutex> lock(g_connect_errors_mu);
        sorted.reserve(g_connect_error_hist.size());
        for (const auto& [code, count] : g_connect_error_hist) {
            sorted.emplace_back(code, count);
        }
    }

    if (sorted.empty()) {
        return;
    }

    std::sort(sorted.begin(), sorted.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });

    std::cout << "Connect Error Types:" << std::endl;
    for (const auto& [code, count] : sorted) {
        const auto io_code = static_cast<IOErrorCode>(code & 0xffffffffu);
        const auto sys_code = static_cast<uint32_t>(code >> 32u);
        IOError error(io_code, sys_code);
        std::cout << "  - count=" << count
                  << " | io=" << static_cast<uint32_t>(io_code)
                  << " | sys=" << sys_code
                  << " | message=" << error.message()
                  << std::endl;
    }
}

// 单个客户端连接的压测协程
Task<void> benchClient(const BenchConfig& config, [[maybe_unused]] int clientId) {
    TcpSocket client;
    client.option().handleNonBlock();

    Host serverHost(IPType::IPV4, config.host, config.port);
    g_connect_attempts.fetch_add(1, std::memory_order_relaxed);
    const auto connect_start = std::chrono::steady_clock::now();
    std::expected<void, IOError> connectResult;
    if (config.connectTimeoutMs > 0) {
        connectResult = co_await client.connect(serverHost).timeout(
            std::chrono::milliseconds(config.connectTimeoutMs));
    } else {
        connectResult = co_await client.connect(serverHost);
    }
    const auto connect_end = std::chrono::steady_clock::now();
    const auto connect_latency_us =
        std::chrono::duration_cast<std::chrono::microseconds>(connect_end - connect_start).count();
    recordConnectLatency(static_cast<uint64_t>(connect_latency_us));

    if (!connectResult) {
        g_error_count.fetch_add(1, std::memory_order_relaxed);
        g_connect_failed.fetch_add(1, std::memory_order_relaxed);
        recordConnectError(connectResult.error());
        if (IOError::contains(connectResult.error().code(), kTimeout)) {
            g_connect_timeout.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_connected_latch) {
            g_connected_latch->arrive();
        }
        (void)co_await client.close();
        co_return;
    }
    g_connect_success.fetch_add(1, std::memory_order_relaxed);

    if (g_connected_latch) {
        g_connected_latch->arrive();
    }
    if (config.connectOnly) {
        (void)co_await client.close();
        co_return;
    }

    if (g_start_gate != nullptr) {
        g_start_gate->wait();
    }

    // 准备测试数据
    std::string message(config.messageSize, 'X');
    char recvBuffer[8192];

    while (g_running.load(std::memory_order_relaxed)) {
        // 发送请求
        auto sendResult = co_await client.send(message.data(), message.size());
        if (!sendResult) {
            g_error_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // 接收响应
        auto recvResult = co_await client.recv(recvBuffer, sizeof(recvBuffer));
        if (!recvResult) {
            g_error_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        size_t bytes = recvResult.value();
        if (bytes == 0) {
            break;
        }

        g_total_requests.fetch_add(1, std::memory_order_relaxed);
        g_total_bytes.fetch_add(bytes + message.size(), std::memory_order_relaxed);
        g_success_count.fetch_add(1, std::memory_order_relaxed);
    }

    co_await client.close();
    co_return;
}

// 统计打印线程
void statsThread(const BenchConfig& config) {
    if (g_connected_latch != nullptr &&
        !g_connected_latch->waitFor(std::chrono::seconds(5))) {
        std::cout << "[warmup] connection gate timed out, starting with available clients" << std::endl;
    }
    if (!config.connectOnly && g_start_gate != nullptr) {
        g_start_gate->open();
    }

    auto startTime = std::chrono::steady_clock::now();
    auto lastTime = startTime;
    uint64_t lastRequests = 0;
    uint64_t lastBytes = 0;
    uint64_t lastConnectCompleted = 0;

    int elapsed_seconds = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        elapsed_seconds++;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();

        uint64_t errors = g_error_count.load(std::memory_order_relaxed);
        if (config.connectOnly) {
            const uint64_t attempts = g_connect_attempts.load(std::memory_order_relaxed);
            const uint64_t success = g_connect_success.load(std::memory_order_relaxed);
            const uint64_t failed = g_connect_failed.load(std::memory_order_relaxed);
            const uint64_t timed_out = g_connect_timeout.load(std::memory_order_relaxed);
            const uint64_t completed = success + failed;
            const double connects_per_sec = (completed - lastConnectCompleted) * 1000.0 / elapsed;

            std::cout << "[" << elapsed_seconds << "s] "
                      << "Connect/s: " << static_cast<uint64_t>(connects_per_sec)
                      << " | Attempts: " << attempts
                      << " | Success: " << success
                      << " | Failed: " << failed
                      << " | Timeout: " << timed_out
                      << " | Errors: " << errors
                      << std::endl;
            lastConnectCompleted = completed;
            if (completed >= static_cast<uint64_t>(config.connections)) {
                g_running.store(false, std::memory_order_release);
                break;
            }
        } else {
            const uint64_t currentRequests = g_total_requests.load(std::memory_order_relaxed);
            const uint64_t currentBytes = g_total_bytes.load(std::memory_order_relaxed);
            const double requestsPerSec = (currentRequests - lastRequests) * 1000.0 / elapsed;
            const double bytesPerSec = (currentBytes - lastBytes) * 1000.0 / elapsed;

            std::cout << "[" << elapsed_seconds << "s] "
                      << "QPS: " << static_cast<uint64_t>(requestsPerSec)
                      << " | Throughput: " << (bytesPerSec / 1024 / 1024) << " MB/s"
                      << " | Total: " << currentRequests
                      << " | Errors: " << errors
                      << std::endl;

            lastRequests = currentRequests;
            lastBytes = currentBytes;
        }

        lastTime = now;

        // 检查是否达到测试时间
        if (elapsed_seconds >= config.duration) {
            g_running.store(false, std::memory_order_release);
            break;
        }
    }
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  -h <host>        Server host (default: 127.0.0.1)\n"
              << "  -p <port>        Server port (default: 8080)\n"
              << "  -c <connections> Number of concurrent connections (default: 100)\n"
              << "  -s <size>        Message size in bytes (default: 256)\n"
              << "  -d <duration>    Test duration in seconds (default: 10)\n"
              << "  --connect-only   Connect and close only, skip request/response loop\n"
              << "  --connect-timeout-ms <ms> Connect timeout in milliseconds (default: 0, disabled)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    BenchConfig config;
    g_running.store(true, std::memory_order_release);
    resetBenchStats();

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            config.host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config.connections = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            config.messageSize = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            config.duration = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "--connect-only") == 0) {
            config.connectOnly = true;
        } else if (strcmp(argv[i], "--connect-timeout-ms") == 0 && i + 1 < argc) {
            config.connectTimeoutMs = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "=== Benchmark Client ===" << std::endl;
    std::cout << "Target: " << config.host << ":" << config.port << std::endl;
    std::cout << "Connections: " << config.connections << std::endl;
    std::cout << "Message Size: " << config.messageSize << " bytes" << std::endl;
    std::cout << "Duration: " << config.duration << " seconds" << std::endl;
    std::cout << "Connect Timeout: " << config.connectTimeoutMs << " ms" << std::endl;
    std::cout << "Mode: " << (config.connectOnly ? "connect-only" : "echo") << std::endl;
    std::cout << "Meta: backend=" << benchmarkBackend()
              << " | build=" << benchmarkBuildMode()
              << " | role=client"
              << " | io_mode=plain"
              << " | scenario=" << (config.connectOnly ? "tcp-connect-only" : "tcp-echo")
              << std::endl;
    std::cout << "========================" << std::endl;

#if defined(USE_KQUEUE)
    std::cout << "Using KqueueScheduler (macOS)" << std::endl;
    KqueueScheduler scheduler;
#elif defined(USE_IOURING)
    std::cout << "Using IOUringScheduler (Linux io_uring)" << std::endl;
    IOUringScheduler scheduler;
#elif defined(USE_EPOLL)
    std::cout << "Using EpollScheduler (Linux epoll)" << std::endl;
    EpollScheduler scheduler;
#else
    LogWarn("No supported IO backend available");
    return 1;
#endif

    scheduler.start();
    galay::benchmark::CompletionLatch connected_latch(static_cast<std::size_t>(config.connections));
    galay::benchmark::StartGate start_gate;
    g_connected_latch = &connected_latch;
    g_start_gate = &start_gate;

    // 启动统计线程
    std::thread stats(statsThread, std::ref(config));

    // 启动所有客户端连接
    std::cout << "Starting " << config.connections << " connections..." << std::endl;
    for (int i = 0; i < config.connections; i++) {
        scheduleTask(scheduler, benchClient(config, i));
    }

    // 等待统计线程结束
    stats.join();
    g_connected_latch = nullptr;
    g_start_gate = nullptr;

    // 等待一下让所有协程完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    scheduler.stop();

    // 打印最终结果
    const uint64_t requests = g_total_requests.load(std::memory_order_relaxed);
    const uint64_t success = g_success_count.load(std::memory_order_relaxed);
    const uint64_t errors = g_error_count.load(std::memory_order_relaxed);
    const uint64_t bytes = g_total_bytes.load(std::memory_order_relaxed);
    const uint64_t connect_attempts = g_connect_attempts.load(std::memory_order_relaxed);
    const uint64_t connect_success = g_connect_success.load(std::memory_order_relaxed);
    const uint64_t connect_failed = g_connect_failed.load(std::memory_order_relaxed);
    const uint64_t connect_timeout = g_connect_timeout.load(std::memory_order_relaxed);
    const auto connect_latency = summarizeConnectLatency();

    std::cout << "\n=== Final Results ===" << std::endl;
    std::cout << "Total Requests: " << requests << std::endl;
    std::cout << "Successful: " << success << std::endl;
    std::cout << "Errors: " << errors << std::endl;
    std::cout << "Total Data: " << (bytes / 1024.0 / 1024.0) << " MB" << std::endl;
    if (!config.connectOnly && config.duration > 0) {
        std::cout << "Average QPS: " << (requests / config.duration) << std::endl;
        std::cout << "Average Throughput: " << (bytes / config.duration / 1024.0 / 1024.0) << " MB/s" << std::endl;
    }

    std::cout << "Connect Attempts: " << connect_attempts << std::endl;
    std::cout << "Connect Success: " << connect_success << std::endl;
    std::cout << "Connect Failed: " << connect_failed << std::endl;
    std::cout << "Connect Timeout: " << connect_timeout << std::endl;
    if (connect_attempts > 0) {
        std::cout << "Connect Success Rate: "
                  << (100.0 * static_cast<double>(connect_success) / static_cast<double>(connect_attempts))
                  << "%" << std::endl;
    }
    std::cout << "Connect Latency(us): "
              << "avg=" << connect_latency.avgUs
              << " p50=" << connect_latency.p50Us
              << " p90=" << connect_latency.p90Us
              << " p99=" << connect_latency.p99Us
              << " max=" << connect_latency.maxUs
              << std::endl;
    printConnectErrorHistogram();

    return 0;
}
