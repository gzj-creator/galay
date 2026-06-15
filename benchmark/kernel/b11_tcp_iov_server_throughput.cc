/**
 * @file b11_iov.cc
 * @brief 用途：作为 `readv/writev` 版本的 TCP 压测服务端，评估 scatter-gather IO 服务路径。
 * 关键覆盖点：双段缓冲回显、连接接受、服务侧吞吐与字节统计。
 * 通过条件：服务端可持续承接压测连接并输出统计，停止后干净退出。
 */

#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include <vector>
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/core/task.h"
#include "benchmark/benchmark_sync.h"
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

size_t fillReadIovecs(std::array<struct iovec, 2>& iovecs,
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

size_t fillWriteIovecsFromRead(std::array<struct iovec, 2>& iovecs,
                               char* prefix,
                               size_t prefixCapacity,
                               char* body,
                               size_t bytesRead) {
    const size_t prefixLen = std::min(bytesRead, prefixCapacity);
    const size_t bodyLen = bytesRead > prefixLen ? bytesRead - prefixLen : 0;

    iovecs[0].iov_base = prefix;
    iovecs[0].iov_len = prefixLen;
    if (bodyLen == 0) {
        return prefixLen == 0 ? 0 : 1;
    }

    iovecs[1].iov_base = body;
    iovecs[1].iov_len = bodyLen;
    return 2;
}

}  // namespace

std::atomic<uint64_t> g_total_connections{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<uint64_t> g_total_requests{0};
std::atomic<bool> g_running{true};

void signalHandler([[maybe_unused]] int signum) {
    g_running.store(false, std::memory_order_release);
}

// 处理单个客户端连接 - 使用用户自管双段 iovec
Task<void> handleClient(GHandle clientHandle) {
    TcpSocket client(clientHandle);
    client.option().handleNonBlock();

    std::array<char, kPrefixBytes> prefix{};
    std::array<char, kBodyBytes> body{};
    std::array<struct iovec, 2> recvIovecs{};
    std::array<struct iovec, 2> sendIovecs{};
    const size_t recvCount = fillReadIovecs(recvIovecs,
                                            prefix.data(),
                                            prefix.size(),
                                            body.data(),
                                            body.size());

    while (g_running.load(std::memory_order_relaxed)) {
        auto recvResult = co_await client.readv(recvIovecs, recvCount);
        if (!recvResult) break;

        size_t bytesRead = recvResult.value();
        if (bytesRead == 0) break;

        g_total_bytes.fetch_add(bytesRead, std::memory_order_relaxed);
        g_total_requests.fetch_add(1, std::memory_order_relaxed);

        const size_t sendCount = fillWriteIovecsFromRead(sendIovecs,
                                                         prefix.data(),
                                                         prefix.size(),
                                                         body.data(),
                                                         bytesRead);
        auto sendResult = co_await client.writev(sendIovecs, sendCount);
        if (!sendResult) break;

        g_total_bytes.fetch_add(sendResult.value(), std::memory_order_relaxed);
    }

    co_await client.close();
    co_return;
}

// 接受连接的协程
Task<void> acceptLoop(IOScheduler* scheduler, TcpSocket* listener) {
    while (g_running.load(std::memory_order_relaxed)) {
        Host clientHost;
        auto acceptResult = co_await listener->accept(&clientHost);
        if (!acceptResult) {
            if (g_running.load(std::memory_order_relaxed)) {
                LogError("Accept failed: {}", acceptResult.error().message());
            }
            continue;
        }

        g_total_connections.fetch_add(1, std::memory_order_relaxed);
        scheduleTask(scheduler, handleClient(acceptResult.value()));
    }
    co_return;
}

// 统计打印线程
void statsThread() {
    auto lastTime = std::chrono::steady_clock::now();
    uint64_t lastBytes = 0;
    uint64_t lastRequests = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();

        uint64_t currentBytes = g_total_bytes.load(std::memory_order_relaxed);
        uint64_t currentRequests = g_total_requests.load(std::memory_order_relaxed);
        uint64_t connections = g_total_connections.load(std::memory_order_relaxed);

        double bytesPerSec = (currentBytes - lastBytes) * 1000.0 / elapsed;
        double requestsPerSec = (currentRequests - lastRequests) * 1000.0 / elapsed;

        std::cout << "[Stats] Connections: " << connections
                  << " | Requests/s: " << static_cast<uint64_t>(requestsPerSec)
                  << " | Throughput: " << (bytesPerSec / 1024 / 1024) << " MB/s"
                  << " | Total Requests: " << currentRequests
                  << std::endl;

        lastTime = now;
        lastBytes = currentBytes;
        lastRequests = currentRequests;
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 8081;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    int scheduler_count =
        galay::benchmark::defaultBenchmarkSchedulerCount(std::thread::hardware_concurrency());
    if (argc > 2) {
        scheduler_count = std::max(1, std::atoi(argv[2]));
    }

    LogInfo("Benchmark IOV Server (readv/writev) starting on port {}", port);
    LogInfo("meta: backend={}, build={}, role=server, io_mode=iov-2seg, scenario=tcp-echo, split={}+rest",
            benchmarkBackend(),
            benchmarkBuildMode(),
            kPrefixBytes);
    LogInfo("scheduler_count={}", scheduler_count);

#if defined(USE_KQUEUE)
    using IOSchedulerType = KqueueScheduler;
    LogInfo("Using KqueueScheduler (macOS)");
#elif defined(USE_IOURING)
    using IOSchedulerType = IOUringScheduler;
    LogInfo("Using IOUringScheduler (Linux io_uring)");
#elif defined(USE_EPOLL)
    using IOSchedulerType = EpollScheduler;
    LogInfo("Using EpollScheduler (Linux epoll)");
#else
    LogWarn("No supported IO backend available");
    return 1;
#endif

    std::vector<std::unique_ptr<IOSchedulerType>> schedulers;
    std::vector<TcpSocket> listeners;
    schedulers.reserve(static_cast<size_t>(scheduler_count));
    listeners.reserve(static_cast<size_t>(scheduler_count));

    Host bindHost(IPType::IPV4, "0.0.0.0", port);
    for (int i = 0; i < scheduler_count; ++i) {
        auto scheduler = std::make_unique<IOSchedulerType>();
        scheduler->start();
        schedulers.push_back(std::move(scheduler));

        TcpSocket listener;
        auto opt = listener.option().handleReuseAddr();
        if (!opt) {
            LogError("Failed to set SO_REUSEADDR: {}", opt.error().message());
            for (auto& running : schedulers) {
                running->stop();
            }
            return 1;
        }
        opt = listener.option().handleReusePort();
        if (!opt) {
            LogError("Failed to set SO_REUSEPORT: {}", opt.error().message());
            for (auto& running : schedulers) {
                running->stop();
            }
            return 1;
        }
        opt = listener.option().handleNonBlock();
        if (!opt) {
            LogError("Failed to set non-block: {}", opt.error().message());
            for (auto& running : schedulers) {
                running->stop();
            }
            return 1;
        }
        opt = listener.option().handleTcpDeferAccept();
        if (!opt) {
            LogError("Failed to set TCP_DEFER_ACCEPT: {}", opt.error().message());
            for (auto& running : schedulers) {
                running->stop();
            }
            return 1;
        }

        auto bindResult = listener.bind(bindHost);
        if (!bindResult) {
            LogError("Failed to bind listener {}: {}", i, bindResult.error().message());
            for (auto& running : schedulers) {
                running->stop();
            }
            return 1;
        }

        auto listenResult = listener.listen(1024);
        if (!listenResult) {
            LogError("Failed to listen on listener {}: {}", i, listenResult.error().message());
            for (auto& running : schedulers) {
                running->stop();
            }
            return 1;
        }

        listeners.push_back(std::move(listener));
    }

    LogInfo("Server listening on 0.0.0.0:{}", port);
    LogInfo("Press Ctrl+C to stop");

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::thread stats(statsThread);
    for (size_t i = 0; i < listeners.size(); ++i) {
        scheduleTask(*schedulers[i], acceptLoop(schedulers[i].get(), &listeners[i]));
    }

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    for (auto& scheduler : schedulers) {
        scheduler->stop();
    }
    stats.join();

    LogInfo("Server stopped. Total connections: {}, Total requests: {}",
            g_total_connections.load(), g_total_requests.load());

    return 0;
}
