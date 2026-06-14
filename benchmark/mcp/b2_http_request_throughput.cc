/**
 * @file B2-HttpPerformance.cc
 * @brief HTTP MCP performance benchmark (concurrent, wrk-like)
 * @details Use multiple connections and concurrent requests to measure throughput and latency.
 */

#include "mcp/client/http_client.h"
#include "kernel/common/sleep.hpp"
#include "kernel/kernel/runtime.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <atomic>
#include <mutex>
#include <cmath>
#include <iomanip>
#include <thread>

using namespace galay::mcp;
using namespace galay::kernel;
using namespace std::chrono;

enum class Operation {
    Ping,
    ToolCall,
    ResourceRead,
    ToolsList,
    ResourcesList,
    PromptsList
};

static const char* operationName(Operation op) {
    switch (op) {
        case Operation::Ping: return "HTTP Ping";
        case Operation::ToolCall: return "HTTP Tool Call";
        case Operation::ResourceRead: return "HTTP Resource Read";
        case Operation::ToolsList: return "HTTP List Tools";
        case Operation::ResourcesList: return "HTTP List Resources";
        case Operation::PromptsList: return "HTTP List Prompts";
        default: return "HTTP Unknown";
    }
}

class ConcurrentStats {
public:
    void addLatency(double latencyMs) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latencies.push_back(latencyMs);
        m_totalTimeMs += latencyMs;
        m_minLatencyMs = std::min(m_minLatencyMs, latencyMs);
        m_maxLatencyMs = std::max(m_maxLatencyMs, latencyMs);
        m_successCount++;
    }

    void addError() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_errorCount++;
    }

    void printReport(const std::string& testName,
                     double totalTestTimeMs,
                     size_t expectedRequests) {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t totalCompleted = m_successCount + m_errorCount;
        size_t totalRequests = expectedRequests > 0 ? expectedRequests : totalCompleted;

        std::cout << "\n=== " << testName << " Concurrent Performance Report ===" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total Requests:    " << totalRequests << std::endl;
        std::cout << "Successful:        " << m_successCount << std::endl;
        std::cout << "Failed:            " << m_errorCount << std::endl;
        if (totalRequests > 0) {
            std::cout << "Success Rate:      " << (m_successCount * 100.0 / totalRequests) << "%" << std::endl;
        }
        std::cout << "Test Duration:     " << totalTestTimeMs << " ms" << std::endl;

        if (m_successCount > 0) {
            double avgLatency = m_totalTimeMs / m_successCount;
            std::cout << "Avg Latency:       " << avgLatency << " ms" << std::endl;
            std::cout << "Min Latency:       " << m_minLatencyMs << " ms" << std::endl;
            std::cout << "Max Latency:       " << m_maxLatencyMs << " ms" << std::endl;

            std::vector<double> sorted = m_latencies;
            std::sort(sorted.begin(), sorted.end());
            size_t mid = sorted.size() / 2;
            double median = sorted.size() % 2 == 0
                ? (sorted[mid - 1] + sorted[mid]) / 2.0
                : sorted[mid];
            std::cout << "Median Latency:    " << median << " ms" << std::endl;

            size_t p95Idx = static_cast<size_t>(sorted.size() * 0.95);
            std::cout << "P95 Latency:       " << sorted[std::min(p95Idx, sorted.size() - 1)] << " ms" << std::endl;

            size_t p99Idx = static_cast<size_t>(sorted.size() * 0.99);
            std::cout << "P99 Latency:       " << sorted[std::min(p99Idx, sorted.size() - 1)] << " ms" << std::endl;

            double sumSquares = 0.0;
            for (double lat : m_latencies) {
                double diff = lat - avgLatency;
                sumSquares += diff * diff;
            }
            double stdDev = std::sqrt(sumSquares / m_latencies.size());
            std::cout << "Std Dev:           " << stdDev << " ms" << std::endl;
        }

        if (totalTestTimeMs > 0) {
            double qps = totalCompleted * 1000.0 / totalTestTimeMs;
            std::cout << "QPS:               " << qps << " req/s" << std::endl;
        }
    }

private:
    std::mutex m_mutex;
    std::vector<double> m_latencies;
    size_t m_successCount = 0;
    size_t m_errorCount = 0;
    double m_totalTimeMs = 0.0;
    double m_minLatencyMs = std::numeric_limits<double>::max();
    double m_maxLatencyMs = 0.0;
};

static void printSystemInfo() {
    std::cout << "\n=== System Information ===" << std::endl;
    std::cout << "Test Date: " << __DATE__ << " " << __TIME__ << std::endl;

    #ifdef __APPLE__
    std::cout << "Platform: macOS" << std::endl;
    #elif __linux__
    std::cout << "Platform: Linux" << std::endl;
    #else
    std::cout << "Platform: Unknown" << std::endl;
    #endif

    std::cout << "Compiler: " << __VERSION__ << std::endl;
    std::cout << "C++ Standard: " << __cplusplus << std::endl;
}

Coroutine workerCoroutine(McpHttpClient& client,
                          const std::string& url,
                          Operation op,
                          size_t requestsPerWorker,
                          ConcurrentStats& stats,
                          std::atomic<int>& readyWorkers,
                          std::atomic<int>& finishedWorkers,
                          std::atomic<int>& disconnectedWorkers,
                          std::atomic<int>& startupFailures,
                          std::atomic<bool>& benchmarkStarted,
                          std::atomic<bool>& benchmarkAborted,
                          size_t workerId) {
    auto connectResult = co_await client.connect(url);
    if (!connectResult) {
        stats.addError();
        startupFailures++;
        finishedWorkers++;
        disconnectedWorkers++;
        co_return;
    }

    std::expected<void, McpError> initResult;
    co_await client.initialize("benchmark-http-client-" + std::to_string(workerId), "1.0.0", initResult);
    if (!initResult) {
        stats.addError();
        startupFailures++;
        finishedWorkers++;
        co_await client.disconnect();
        disconnectedWorkers++;
        co_return;
    }

    readyWorkers++;

    while (!benchmarkStarted.load(std::memory_order_acquire) &&
           !benchmarkAborted.load(std::memory_order_acquire)) {
        co_await sleep(std::chrono::milliseconds(1));
    }

    if (benchmarkAborted.load(std::memory_order_acquire)) {
        finishedWorkers++;
        co_await client.disconnect();
        disconnectedWorkers++;
        co_return;
    }

    for (size_t i = 0; i < requestsPerWorker; ++i) {
        auto start = high_resolution_clock::now();
        bool ok = false;

        switch (op) {
            case Operation::Ping: {
                std::expected<void, McpError> pingResult;
                co_await client.ping(pingResult);
                ok = pingResult.has_value();
                break;
            }
            case Operation::ToolCall: {
                JsonWriter argsWriter;
                argsWriter.StartObject();
                argsWriter.Key("message");
                argsWriter.String("Benchmark test message " + std::to_string(i));
                argsWriter.EndObject();
                JsonString args = argsWriter.TakeString();
                std::expected<JsonString, McpError> callResult;
                co_await client.callTool("echo", args, callResult);
                ok = callResult.has_value();
                break;
            }
            case Operation::ResourceRead: {
                std::expected<std::string, McpError> readResult;
                co_await client.readResource("example://hello", readResult);
                ok = readResult.has_value();
                break;
            }
            case Operation::ToolsList: {
                std::expected<std::vector<Tool>, McpError> result;
                co_await client.listTools(result);
                ok = result.has_value();
                break;
            }
            case Operation::ResourcesList: {
                std::expected<std::vector<Resource>, McpError> result;
                co_await client.listResources(result);
                ok = result.has_value();
                break;
            }
            case Operation::PromptsList: {
                std::expected<std::vector<Prompt>, McpError> result;
                co_await client.listPrompts(result);
                ok = result.has_value();
                break;
            }
        }

        auto end = high_resolution_clock::now();
        if (ok) {
            double latencyMs = duration_cast<microseconds>(end - start).count() / 1000.0;
            stats.addLatency(latencyMs);
        } else {
            stats.addError();
        }
    }

    finishedWorkers++;
    co_await client.disconnect();
    disconnectedWorkers++;
    co_return;
}

static void runConcurrentTest(Runtime& runtime,
                              std::vector<std::unique_ptr<McpHttpClient>>& clients,
                              const std::string& url,
                              Operation op,
                              size_t requestsPerWorker) {
    size_t numWorkers = clients.size();
    size_t totalRequests = numWorkers * requestsPerWorker;

    std::cout << "\n=== Concurrent Test ===" << std::endl;
    std::cout << "Operation:         " << operationName(op) << std::endl;
    std::cout << "Connections:       " << numWorkers << std::endl;
    std::cout << "Requests/Conn:     " << requestsPerWorker << std::endl;
    std::cout << "Total Requests:    " << totalRequests << std::endl;
    std::cout << "\nStarting test..." << std::endl;

    ConcurrentStats stats;
    std::atomic<int> readyWorkers(0);
    std::atomic<int> finishedWorkers(0);
    std::atomic<int> disconnectedWorkers(0);
    std::atomic<int> startupFailures(0);
    std::atomic<bool> benchmarkStarted(false);
    std::atomic<bool> benchmarkAborted(false);

    for (size_t i = 0; i < numWorkers; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler ||
            !scheduleTask(scheduler,
                          workerCoroutine(*clients[i],
                                          url,
                                          op,
                                          requestsPerWorker,
                                          stats,
                                          readyWorkers,
                                          finishedWorkers,
                                          disconnectedWorkers,
                                          startupFailures,
                                          benchmarkStarted,
                                          benchmarkAborted,
                                          i))) {
            std::cerr << "Failed to schedule worker " << i << std::endl;
            stats.addError();
            startupFailures++;
            finishedWorkers++;
            disconnectedWorkers++;
        }
    }

    const auto deadline = high_resolution_clock::now() + std::chrono::seconds(30);
    while (readyWorkers.load(std::memory_order_acquire) + startupFailures.load(std::memory_order_acquire) <
               static_cast<int>(numWorkers) &&
           high_resolution_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (readyWorkers.load(std::memory_order_acquire) + startupFailures.load(std::memory_order_acquire) <
            static_cast<int>(numWorkers) ||
        startupFailures.load(std::memory_order_acquire) != 0) {
        benchmarkAborted.store(true, std::memory_order_release);
        while (disconnectedWorkers.load(std::memory_order_acquire) < static_cast<int>(numWorkers) &&
               high_resolution_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cerr << "Benchmark startup failed for " << operationName(op) << std::endl;
        return;
    }

    benchmarkStarted.store(true, std::memory_order_release);
    const auto testStart = high_resolution_clock::now();

    while (finishedWorkers.load(std::memory_order_acquire) < static_cast<int>(numWorkers) &&
           high_resolution_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto testEnd = high_resolution_clock::now();
    const double totalTestTimeMs =
        duration_cast<microseconds>(testEnd - testStart).count() / 1000.0;

    while (disconnectedWorkers.load(std::memory_order_acquire) < static_cast<int>(numWorkers) &&
           high_resolution_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stats.printReport(operationName(op), totalTestTimeMs, totalRequests);
}

static void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  --url <url>           Server URL (default: http://127.0.0.1:8080/mcp)\n";
    std::cout << "  --connections <n>     Number of concurrent connections (default: 8)\n";
    std::cout << "  --requests <n>        Requests per connection per test (default: 2000)\n";
    std::cout << "  --io <n>              IO scheduler count (default: 2)\n";
    std::cout << "  --compute <n>         Compute scheduler count (default: 0)\n";
    std::cout << "  --help                Show this help message\n";
}

int main(int argc, char* argv[]) {
    std::string url = "http://127.0.0.1:8080/mcp";
    size_t connections = 8;
    size_t requestsPerConn = 2000;
    size_t ioSchedulers = 2;
    size_t computeSchedulers = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if ((arg == "--connections" || arg == "--workers") && i + 1 < argc) {
            connections = std::stoul(argv[++i]);
        } else if (arg == "--requests" && i + 1 < argc) {
            requestsPerConn = std::stoul(argv[++i]);
        } else if (arg == "--io" && i + 1 < argc) {
            ioSchedulers = std::stoul(argv[++i]);
        } else if (arg == "--compute" && i + 1 < argc) {
            computeSchedulers = std::stoul(argv[++i]);
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    printSystemInfo();

    std::cout << "\n=== HTTP MCP Performance Benchmark (Concurrent) ===" << std::endl;
    std::cout << "Server URL:        " << url << std::endl;
    std::cout << "Connections:       " << connections << std::endl;
    std::cout << "Requests/Conn:     " << requestsPerConn << std::endl;
    std::cout << "IO Schedulers:     " << ioSchedulers << std::endl;
    std::cout << "Compute Schedulers:" << computeSchedulers << std::endl;
    std::cout << "Make sure the HTTP MCP server is running!" << std::endl;

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(ioSchedulers).computeSchedulerCount(computeSchedulers).build();
    runtime.start();

    std::vector<std::unique_ptr<McpHttpClient>> clients;
    clients.reserve(connections);
    for (size_t i = 0; i < connections; ++i) {
        clients.push_back(std::make_unique<McpHttpClient>(runtime));
    }

    runConcurrentTest(runtime, clients, url, Operation::Ping, requestsPerConn);
    runConcurrentTest(runtime, clients, url, Operation::ToolCall, requestsPerConn);
    runConcurrentTest(runtime, clients, url, Operation::ResourceRead, requestsPerConn);
    runConcurrentTest(runtime, clients, url, Operation::ToolsList, requestsPerConn);
    runConcurrentTest(runtime, clients, url, Operation::ResourcesList, requestsPerConn);
    runConcurrentTest(runtime, clients, url, Operation::PromptsList, requestsPerConn);

    runtime.stop();

    std::cout << "\n=== Benchmark Complete ===" << std::endl;
    return 0;
}
