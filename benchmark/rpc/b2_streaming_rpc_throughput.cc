/**
 * @file b2_rpc.cpp
 * @brief RPC压测客户端（含P99延迟统计）
 */

#include "galay-rpc/kernel/rpc_conn.h"
#include "galay-rpc/utils/runtime_compat.h"
#include "galay-kernel/common/sleep.hpp"
#include "galay-kernel/core/runtime.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>
#include <csignal>
#include <iomanip>
#include <algorithm>
#include <mutex>
#include <optional>
#include <string_view>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {
constexpr int kDefaultPort = 9000;
constexpr size_t kDefaultConnections = 100;
constexpr size_t kDefaultPayloadSize = 256;
constexpr size_t kDefaultDurationSec = 10;
constexpr size_t kDefaultIoSchedulers = 0;
constexpr size_t kDefaultPipelineDepth = 1;

constexpr int kMaxConnectRetries = 15;
constexpr auto kRetryInterval = std::chrono::seconds(1);
constexpr size_t kClientRingBufferHeadroom = 256;
constexpr size_t kLatencyReserve = 10000;

const char* callModeToString(RpcCallMode mode) {
    switch (mode) {
        case RpcCallMode::UNARY:
            return "unary";
        case RpcCallMode::CLIENT_STREAMING:
            return "client_stream";
        case RpcCallMode::SERVER_STREAMING:
            return "server_stream";
        case RpcCallMode::BIDI_STREAMING:
            return "bidi";
        default:
            return "unknown";
    }
}

std::optional<RpcCallMode> parseCallMode(std::string_view mode) {
    if (mode == "unary") {
        return RpcCallMode::UNARY;
    }
    if (mode == "client_stream") {
        return RpcCallMode::CLIENT_STREAMING;
    }
    if (mode == "server_stream") {
        return RpcCallMode::SERVER_STREAMING;
    }
    if (mode == "bidi") {
        return RpcCallMode::BIDI_STREAMING;
    }
    return std::nullopt;
}
}

struct BenchConfig {
    std::string host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    size_t connections = kDefaultConnections;
    size_t payload_size = kDefaultPayloadSize;
    size_t duration_sec = kDefaultDurationSec;
    size_t io_schedulers = kDefaultIoSchedulers;
    size_t pipeline_depth = kDefaultPipelineDepth;
    RpcCallMode mode = RpcCallMode::UNARY;
};

std::atomic<uint64_t> g_total_requests{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<bool> g_running{true};

// 延迟统计
std::mutex g_latency_mutex;
std::vector<uint64_t> g_latencies;  // 微秒

void signalHandler(int) {
    g_running.store(false);
}

Task<void> benchWorker(const BenchConfig& config) {
    const size_t ring_buffer_size =
        std::max<size_t>(kDefaultRpcRingBufferSize, config.payload_size + RPC_HEADER_SIZE + kClientRingBufferHeadroom);
    const size_t pipeline_depth = std::max<size_t>(1, config.pipeline_depth);
    std::string payload(config.payload_size, 'X');
    std::vector<uint64_t> local_latencies;
    local_latencies.reserve(kLatencyReserve);

    while (g_running.load(std::memory_order_relaxed)) {
        RpcConn conn(IPType::IPV4, RpcReaderSetting{}, RpcWriterSetting{}, ring_buffer_size);

        bool connected = false;
        for (int retry_count = 0; retry_count < kMaxConnectRetries && g_running.load(std::memory_order_relaxed); ++retry_count) {
            Host server_host(IPType::IPV4, config.host, config.port);
            auto connect_result = co_await conn.connect(server_host);
            if (connect_result.has_value()) {
                connected = true;
                break;
            }

            (void)co_await conn.close();

            if (retry_count + 1 < kMaxConnectRetries && g_running.load(std::memory_order_relaxed)) {
                co_await sleep(kRetryInterval);
            }
        }

        if (!connected) {
            break;
        }

        auto reader = conn.getReader();
        auto writer = conn.getWriter();
        struct InflightEntry {
            uint32_t request_id;
            std::chrono::steady_clock::time_point send_time;
        };
        std::vector<InflightEntry> inflight_entries;
        inflight_entries.reserve(pipeline_depth);
        uint32_t next_request_id = 0;
        bool reconnect_needed = false;

        while (g_running.load(std::memory_order_relaxed) && !reconnect_needed) {
            while (g_running.load(std::memory_order_relaxed) &&
                   inflight_entries.size() < pipeline_depth &&
                   !reconnect_needed) {
                RpcRequest request(next_request_id++, "BenchEchoService", "echo");
                request.callMode(config.mode);
                request.endOfStream(true);
                request.payloadView(RpcPayloadView{
                    payload.data(),
                    payload.size(),
                    nullptr,
                    0
                });
                const auto send_start = std::chrono::steady_clock::now();

                auto send_result = co_await writer.sendRequest(request);
                if (!send_result.has_value()) {
                    reconnect_needed = true;
                } else {
                    inflight_entries.push_back(InflightEntry{
                        request.requestId(),
                        send_start
                    });
                }
            }

            if (reconnect_needed || inflight_entries.empty()) {
                continue;
            }

            RpcResponse response;
            auto recv_result = co_await reader.getResponse(response);
            if (!recv_result.has_value()) {
                reconnect_needed = true;
            }

            if (reconnect_needed) {
                break;
            }

            auto entry_it = std::find_if(
                inflight_entries.begin(),
                inflight_entries.end(),
                [&](const InflightEntry& entry) { return entry.request_id == response.requestId(); });
            if (entry_it == inflight_entries.end()) {
                reconnect_needed = true;
                break;
            }

            const auto end = std::chrono::steady_clock::now();
            const auto latency_us =
                std::chrono::duration_cast<std::chrono::microseconds>(end - entry_it->send_time).count();
            local_latencies.push_back(static_cast<uint64_t>(latency_us));

            *entry_it = std::move(inflight_entries.back());
            inflight_entries.pop_back();

            if (response.isOk()) {
                g_total_requests.fetch_add(1, std::memory_order_relaxed);
                g_total_bytes.fetch_add(payload.size() * 2, std::memory_order_relaxed);
            }
        }

        (void)co_await conn.close();

        if (g_running.load(std::memory_order_relaxed) && reconnect_needed) {
            co_await sleep(kRetryInterval);
        }
    }

    // 合并本地延迟数据
    if (!local_latencies.empty()) {
        std::lock_guard<std::mutex> lock(g_latency_mutex);
        g_latencies.insert(g_latencies.end(), local_latencies.begin(), local_latencies.end());
    }
    co_return;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -h <host>        Server host (default: 127.0.0.1)\n"
              << "  -p <port>        Server port (default: 9000)\n"
              << "  -c <connections> Number of connections (default: 100)\n"
              << "  -s <size>        Payload size in bytes (default: 256)\n"
              << "  -d <duration>    Test duration in seconds (default: 10)\n"
              << "  -i <io_count>    IO scheduler count (default: auto, 0)\n"
              << "  -l <pipeline>    Pipeline depth per connection (default: 1)\n"
              << "  -m <mode>        RPC mode: unary|client_stream|server_stream|bidi (default: unary)\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#if defined(SIGPIPE)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    BenchConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];

        if (opt == "--help") {
            printUsage(argv[0]);
            return 0;
        }

        if (i + 1 >= argc) {
            std::cerr << "Missing value for option: " << opt << "\n";
            printUsage(argv[0]);
            return 1;
        }

        std::string val = argv[++i];

        if (opt == "-h") config.host = val;
        else if (opt == "-p") config.port = static_cast<uint16_t>(std::stoi(val));
        else if (opt == "-c") config.connections = std::stoul(val);
        else if (opt == "-s") config.payload_size = std::stoul(val);
        else if (opt == "-d") config.duration_sec = std::stoul(val);
        else if (opt == "-i") config.io_schedulers = std::stoul(val);
        else if (opt == "-l") config.pipeline_depth = std::stoul(val);
        else if (opt == "-m") {
            auto mode = parseCallMode(val);
            if (!mode.has_value()) {
                std::cerr << "Invalid mode: " << val << "\n";
                printUsage(argv[0]);
                return 1;
            }
            config.mode = mode.value();
        } else {
            std::cerr << "Unknown option: " << opt << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    std::cout << "=== RPC Benchmark Client ===\n";
    std::cout << "Target: " << config.host << ":" << config.port << "\n";
    std::cout << "Connections: " << config.connections << "\n";
    std::cout << "Payload size: " << config.payload_size << " bytes\n";
    std::cout << "Duration: " << config.duration_sec << " seconds\n";
    const size_t resolved_io_schedulers = resolveIoSchedulerCount(config.io_schedulers);
    std::cout << "IO Schedulers: "
              << (config.io_schedulers == 0
                      ? "auto (" + std::to_string(resolved_io_schedulers) + ")"
                      : std::to_string(resolved_io_schedulers))
              << "\n";
    std::cout << "Pipeline depth: " << std::max<size_t>(1, config.pipeline_depth) << "\n";
    std::cout << "RPC mode: " << callModeToString(config.mode) << "\n";
    std::cout << "\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(resolved_io_schedulers).computeSchedulerCount(1).build();
    runtime.start();

    // 启动所有连接
    std::cout << "Starting " << config.connections << " connections...\n";
    bool schedule_failed = false;
    for (size_t i = 0; i < config.connections; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (scheduler == nullptr || !scheduleTask(scheduler, benchWorker(config))) {
            schedule_failed = true;
            break;
        }
    }
    if (schedule_failed) {
        std::cerr << "Failed to schedule benchmark workers on IO scheduler(s)\n";
        g_running.store(false, std::memory_order_relaxed);
        runtime.stop();
        return 1;
    }

    std::cout << "Benchmark running...\n\n";

    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_requests = 0;
    uint64_t last_bytes = 0;

    for (size_t sec = 0; sec < config.duration_sec && g_running.load(); ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t current_requests = g_total_requests.load();
        uint64_t current_bytes = g_total_bytes.load();

        uint64_t qps = current_requests - last_requests;
        double throughput_mb = static_cast<double>(current_bytes - last_bytes) / (1024.0 * 1024.0);

        std::cout << "[" << std::setw(3) << (sec + 1) << "s] "
                  << "QPS: " << std::setw(8) << qps
                  << " | Throughput: " << std::fixed << std::setprecision(2) << throughput_mb << " MB/s"
                  << "\n";

        last_requests = current_requests;
        last_bytes = current_bytes;
    }

    g_running.store(false);
    auto end_time = std::chrono::steady_clock::now();

    // 等待连接关闭
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    runtime.stop();

    // 计算统计
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double duration_sec = duration.count() / 1000.0;

    uint64_t total_requests = g_total_requests.load();
    uint64_t total_bytes = g_total_bytes.load();

    double avg_qps = total_requests / duration_sec;
    double avg_throughput = (total_bytes / (1024.0 * 1024.0)) / duration_sec;

    std::cout << "\n=== Benchmark Results ===\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << duration_sec << " seconds\n";
    std::cout << "Total Requests: " << total_requests << "\n";
    std::cout << "Average QPS: " << std::fixed << std::setprecision(0) << avg_qps << "\n";
    std::cout << "Average Throughput: " << std::fixed << std::setprecision(2) << avg_throughput << " MB/s\n";

    // 计算延迟百分位数
    if (!g_latencies.empty()) {
        std::sort(g_latencies.begin(), g_latencies.end());
        size_t n = g_latencies.size();

        auto p50 = g_latencies[n * 50 / 100];
        auto p90 = g_latencies[n * 90 / 100];
        auto p95 = g_latencies[n * 95 / 100];
        auto p99 = g_latencies[n * 99 / 100];
        auto p999 = g_latencies[std::min(n * 999 / 1000, n - 1)];
        auto max_lat = g_latencies[n - 1];

        uint64_t sum = 0;
        for (auto lat : g_latencies) sum += lat;
        double avg_lat = static_cast<double>(sum) / n;

        std::cout << "\n=== Latency (microseconds) ===\n";
        std::cout << "Samples: " << n << "\n";
        std::cout << "Avg:  " << std::fixed << std::setprecision(0) << avg_lat << " us\n";
        std::cout << "P50:  " << p50 << " us\n";
        std::cout << "P90:  " << p90 << " us\n";
        std::cout << "P95:  " << p95 << " us\n";
        std::cout << "P99:  " << p99 << " us\n";
        std::cout << "P99.9: " << p999 << " us\n";
        std::cout << "Max:  " << max_lat << " us\n";
    }

    return 0;
}
