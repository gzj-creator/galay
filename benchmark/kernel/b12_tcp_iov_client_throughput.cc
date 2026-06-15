/**
 * @file b12_iov.cc
 * @brief 用途：作为 `readv/writev` 版本的 TCP 压测客户端，评估 scatter-gather IO 客户路径。
 * 关键覆盖点：双段缓冲请求发送、响应接收、样本同步与吞吐延迟统计。
 * 通过条件：客户端完成既定负载并输出统计结果，进程干净退出。
 */

#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include "benchmark/benchmark_sync.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/core/task.h"
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/core/kqueue_scheduler.h"
#elif defined(USE_IOURING)
#include "galay-kernel/core/uring_scheduler.h"
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
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

constexpr size_t kPrefixBytes = 64;
constexpr size_t kBodyBytes = 8192;

size_t fillMessageIovecs(std::array<struct iovec, 2>& iovecs, std::string_view message) {
    const size_t prefixLen = std::min(message.size(), kPrefixBytes);
    const size_t bodyLen = message.size() > prefixLen ? message.size() - prefixLen : 0;

    iovecs[0].iov_base = const_cast<char*>(message.data());
    iovecs[0].iov_len = prefixLen;
    if (bodyLen == 0) {
        return prefixLen == 0 ? 0 : 1;
    }

    iovecs[1].iov_base = const_cast<char*>(message.data() + prefixLen);
    iovecs[1].iov_len = bodyLen;
    return 2;
}

size_t fillResponseIovecs(std::array<struct iovec, 2>& iovecs,
                          char* prefix,
                          size_t prefixLen,
                          char* body,
                          size_t bodyLen) {
    iovecs[0].iov_base = prefix;
    iovecs[0].iov_len = prefixLen;
    iovecs[1].iov_base = body;
    iovecs[1].iov_len = bodyLen;
    return bodyLen == 0 ? 1 : 2;
}

}  // namespace

std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<uint64_t> g_success_count{0};
std::atomic<uint64_t> g_error_count{0};
std::atomic<bool> g_running{true};
galay::benchmark::CompletionLatch* g_connected_latch = nullptr;
galay::benchmark::StartGate* g_start_gate = nullptr;

struct BenchConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 8081;
    int connections = 100;
    int messageSize = 256;
    int duration = 10;  // seconds
};

// 单个客户端连接的压测协程 - 使用用户自管双段 iovec
Task<void> benchClient(const BenchConfig& config, [[maybe_unused]] int clientId) {
    TcpSocket client;
    client.option().handleNonBlock();

    Host serverHost(IPType::IPV4, config.host, config.port);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        g_error_count.fetch_add(1, std::memory_order_relaxed);
        if (g_connected_latch) {
            g_connected_latch->arrive();
        }
        co_return;
    }

    if (g_connected_latch) {
        g_connected_latch->arrive();
    }
    if (g_start_gate) {
        g_start_gate->wait();
    }

    std::array<struct iovec, 2> writeIovecs{};
    std::array<struct iovec, 2> readIovecs{};
    std::array<char, kPrefixBytes> respPrefix{};
    std::array<char, kBodyBytes> respBody{};

    // 准备测试数据
    std::string message(config.messageSize, 'X');
    const size_t writeCount = fillMessageIovecs(writeIovecs, message);
    const size_t readCount = fillResponseIovecs(readIovecs,
                                                respPrefix.data(),
                                                respPrefix.size(),
                                                respBody.data(),
                                                respBody.size());

    while (g_running.load(std::memory_order_relaxed)) {
        // 使用 writev 发送
        auto sendResult = co_await client.writev(writeIovecs, writeCount);
        if (!sendResult) {
            g_error_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // 使用 readv 接收
        auto recvResult = co_await client.readv(readIovecs, readCount);
        if (!recvResult) {
            g_error_count.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        size_t bytesRead = recvResult.value();
        if (bytesRead == 0) {
            break;
        }

        g_total_requests.fetch_add(1, std::memory_order_relaxed);
        g_total_bytes.fetch_add(message.size() + bytesRead, std::memory_order_relaxed);
        g_success_count.fetch_add(1, std::memory_order_relaxed);
    }

    co_await client.close();
    co_return;
}

// 统计打印线程
void statsThread(const BenchConfig& config) {
    if (g_connected_latch && !g_connected_latch->waitFor(std::chrono::seconds(5))) {
        std::cout << "[warmup] connection gate timed out, starting with available clients" << std::endl;
    }
    if (g_start_gate) {
        g_start_gate->open();
    }

    auto startTime = std::chrono::steady_clock::now();
    auto lastTime = startTime;
    uint64_t lastRequests = 0;
    uint64_t lastBytes = 0;

    int elapsed_seconds = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        elapsed_seconds++;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();

        uint64_t currentRequests = g_total_requests.load(std::memory_order_relaxed);
        uint64_t currentBytes = g_total_bytes.load(std::memory_order_relaxed);
        uint64_t errors = g_error_count.load(std::memory_order_relaxed);

        double requestsPerSec = (currentRequests - lastRequests) * 1000.0 / elapsed;
        double bytesPerSec = (currentBytes - lastBytes) * 1000.0 / elapsed;

        std::cout << "[" << elapsed_seconds << "s] "
                  << "QPS: " << static_cast<uint64_t>(requestsPerSec)
                  << " | Throughput: " << (bytesPerSec / 1024 / 1024) << " MB/s"
                  << " | Total: " << currentRequests
                  << " | Errors: " << errors
                  << std::endl;

        lastTime = now;
        lastRequests = currentRequests;
        lastBytes = currentBytes;

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
              << "  -p <port>        Server port (default: 8081)\n"
              << "  -c <connections> Number of concurrent connections (default: 100)\n"
              << "  -s <size>        Message size in bytes (default: 256)\n"
              << "  -d <duration>    Test duration in seconds (default: 10)\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    BenchConfig config;
    g_running.store(true, std::memory_order_release);

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
        } else if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
    }

    std::cout << "=== Benchmark IOV Client (readv/writev) ===" << std::endl;
    std::cout << "Target: " << config.host << ":" << config.port << std::endl;
    std::cout << "Connections: " << config.connections << std::endl;
    std::cout << "Message Size: " << config.messageSize << " bytes" << std::endl;
    std::cout << "Duration: " << config.duration << " seconds" << std::endl;
    std::cout << "Meta: backend=" << benchmarkBackend()
              << " | build=" << benchmarkBuildMode()
              << " | role=client"
              << " | io_mode=iov-2seg"
              << " | scenario=tcp-echo"
              << " | split=" << kPrefixBytes << "+rest" << std::endl;
    std::cout << "===========================================" << std::endl;

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

    std::thread stats(statsThread, std::ref(config));

    std::cout << "Starting " << config.connections << " connections..." << std::endl;
    for (int i = 0; i < config.connections; i++) {
        scheduleTask(scheduler, benchClient(config, i));
    }

    stats.join();
    g_connected_latch = nullptr;
    g_start_gate = nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    scheduler.stop();

    std::cout << "\n=== Final Results ===" << std::endl;
    std::cout << "Total Requests: " << g_total_requests.load() << std::endl;
    std::cout << "Successful: " << g_success_count.load() << std::endl;
    std::cout << "Errors: " << g_error_count.load() << std::endl;
    std::cout << "Total Data: " << (g_total_bytes.load() / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "Average QPS: " << (g_total_requests.load() / config.duration) << std::endl;
    std::cout << "Average Throughput: " << (g_total_bytes.load() / config.duration / 1024.0 / 1024.0) << " MB/s" << std::endl;

    return 0;
}
