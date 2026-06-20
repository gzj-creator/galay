/**
 * @file b5_stream.cpp
 * @brief 真实流式 RPC 压测客户端
 */

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/utils/runtime_compat.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {
constexpr uint16_t kDefaultPort = 9100;
constexpr size_t kDefaultConnections = 100;
constexpr size_t kDefaultPayloadSize = 128;
constexpr size_t kDefaultFramesPerStream = 16;
constexpr size_t kDefaultFrameWindow = 1;
constexpr size_t kDefaultDurationSec = 10;
constexpr size_t kDefaultIoSchedulers = 0;

constexpr int kMaxConnectRetries = 15;
constexpr auto kRetryInterval = std::chrono::seconds(1);
constexpr size_t kLatencyReserve = 10000;
// Batch global latency aggregation to avoid turning the benchmark itself into the bottleneck.
constexpr size_t kLatencyFlushBatch = 1024;

struct BenchConfig {
    std::string host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    size_t connections = kDefaultConnections;
    size_t payload_size = kDefaultPayloadSize;
    size_t frames_per_stream = kDefaultFramesPerStream;
    size_t frame_window = kDefaultFrameWindow;
    size_t duration_sec = kDefaultDurationSec;
    size_t io_schedulers = kDefaultIoSchedulers;
};

std::atomic<uint64_t> g_total_streams{0};
std::atomic<uint64_t> g_total_frames{0};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<bool> g_running{true};
std::atomic<size_t> g_workers_finished{0};

std::mutex g_latency_mutex;
std::vector<uint64_t> g_stream_latencies; // us

void signalHandler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

Task<void> benchWorker(const BenchConfig& config, uint32_t worker_id) {
    const size_t client_ring_buffer_size =
        std::max<size_t>(kDefaultRpcRingBufferSize,
                         config.payload_size * config.frames_per_stream * 2 + RPC_HEADER_SIZE * 64);

    std::string payload(config.payload_size, 'S');
    const RpcPayloadView payload_view{
        payload.data(),
        payload.size(),
        nullptr,
        0
    };
    std::vector<uint64_t> local_latencies;
    local_latencies.reserve(kLatencyReserve);
    auto flushLocalLatencies = [&local_latencies]() {
        if (local_latencies.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_latency_mutex);
        g_stream_latencies.insert(g_stream_latencies.end(), local_latencies.begin(), local_latencies.end());
        local_latencies.clear();
    };

    uint32_t next_stream_id = ((worker_id + 1) << 24);

    while (g_running.load(std::memory_order_relaxed)) {
        auto client = RpcClientBuilder()
            .ringBufferSize(client_ring_buffer_size)
            .build();

        bool connected = false;
        for (int retry_count = 0;
             retry_count < kMaxConnectRetries && g_running.load(std::memory_order_relaxed);
             ++retry_count) {
            auto connect_result = co_await client.connect(config.host, config.port);
            if (connect_result.has_value()) {
                connected = true;
                break;
            }

            (void)co_await client.close();

            if (retry_count + 1 < kMaxConnectRetries && g_running.load(std::memory_order_relaxed)) {
                co_await sleep(kRetryInterval);
            }
        }

        if (!connected) {
            break;
        }

        bool reconnect_needed = false;
        auto stream_result = client.createStream(next_stream_id++, "StreamBenchService", "echo");
        if (!stream_result.has_value()) {
            break;
        }
        auto stream = stream_result.value();
        StreamMessage init_ack;
        StreamMessage echo_frame;
        StreamMessage tail_frame;

        while (g_running.load(std::memory_order_relaxed) && !reconnect_needed) {
            const uint32_t stream_id = next_stream_id++;
            stream.streamId(stream_id);

            const auto stream_start = std::chrono::steady_clock::now();

            auto send_result = co_await stream.sendInit();
            if (!send_result.has_value()) {
                reconnect_needed = true;
            }
            if (reconnect_needed) {
                break;
            }

            auto recv_result = co_await stream.read(init_ack);
            if (!recv_result.has_value()) {
                reconnect_needed = true;
            }
            if (reconnect_needed) {
                break;
            }

            if (init_ack.messageType() != RpcMessageType::STREAM_INIT_ACK ||
                init_ack.streamId() != stream_id) {
                reconnect_needed = true;
                break;
            }

            const size_t frame_window = std::max<size_t>(1, std::min(config.frame_window, config.frames_per_stream));
            size_t sent_frames = 0;
            size_t recv_frames = 0;
            while (recv_frames < config.frames_per_stream && g_running.load(std::memory_order_relaxed)) {
                while (sent_frames < config.frames_per_stream &&
                       sent_frames - recv_frames < frame_window &&
                       g_running.load(std::memory_order_relaxed)) {
                    send_result = co_await stream.sendData(payload_view);
                    if (!send_result.has_value()) {
                        reconnect_needed = true;
                    }
                    if (reconnect_needed) {
                        break;
                    }
                    ++sent_frames;
                }
                if (reconnect_needed) {
                    break;
                }

                if (recv_frames >= sent_frames) {
                    continue;
                }

                recv_result = co_await stream.read(echo_frame);
                if (!recv_result.has_value()) {
                    reconnect_needed = true;
                }
                if (reconnect_needed) {
                    break;
                }

                if (echo_frame.messageType() != RpcMessageType::STREAM_DATA ||
                    echo_frame.streamId() != stream_id ||
                    !echo_frame.payloadEquals(payload)) {
                    reconnect_needed = true;
                    break;
                }
                ++recv_frames;
            }

            if (reconnect_needed || !g_running.load(std::memory_order_relaxed)) {
                break;
            }

            if (recv_frames != config.frames_per_stream) {
                reconnect_needed = true;
                break;
            }

            send_result = co_await stream.sendEnd();
            if (!send_result.has_value()) {
                reconnect_needed = true;
            }
            if (reconnect_needed) {
                break;
            }

            bool got_end = false;
            while (!got_end) {
                recv_result = co_await stream.read(tail_frame);
                if (!recv_result.has_value()) {
                    reconnect_needed = true;
                }

                if (reconnect_needed) {
                    break;
                }

                if (tail_frame.messageType() == RpcMessageType::STREAM_END) {
                    got_end = true;
                } else if (tail_frame.messageType() == RpcMessageType::STREAM_CANCEL) {
                    reconnect_needed = true;
                    break;
                }
            }

            if (reconnect_needed) {
                break;
            }

            const auto stream_end = std::chrono::steady_clock::now();
            const auto latency_us =
                std::chrono::duration_cast<std::chrono::microseconds>(stream_end - stream_start).count();
            local_latencies.push_back(static_cast<uint64_t>(latency_us));
            if (local_latencies.size() >= kLatencyFlushBatch) {
                flushLocalLatencies();
            }

            g_total_streams.fetch_add(1, std::memory_order_relaxed);
            g_total_frames.fetch_add(config.frames_per_stream, std::memory_order_relaxed);
            g_total_bytes.fetch_add(config.payload_size * config.frames_per_stream * 2,
                                    std::memory_order_relaxed);
        }

        (void)co_await client.close();

        if (g_running.load(std::memory_order_relaxed) && reconnect_needed) {
            co_await sleep(kRetryInterval);
        }
    }

    flushLocalLatencies();
    g_workers_finished.fetch_add(1, std::memory_order_relaxed);

    co_return;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -h <host>        Server host (default: 127.0.0.1)\n"
              << "  -p <port>        Server port (default: 9100)\n"
              << "  -c <connections> Number of connections (default: 100)\n"
              << "  -s <size>        Payload size in bytes (default: 128)\n"
              << "  -f <frames>      Frames per stream (default: 16)\n"
              << "  -w <window>      Frame pipeline window per stream (default: 1)\n"
              << "  -d <duration>    Test duration in seconds (default: 10)\n"
              << "  -i <io_count>    IO scheduler count (default: auto, 0)\n";
}
} // namespace

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
        else if (opt == "-f") config.frames_per_stream = std::max<size_t>(1, std::stoul(val));
        else if (opt == "-w") config.frame_window = std::max<size_t>(1, std::stoul(val));
        else if (opt == "-d") config.duration_sec = std::stoul(val);
        else if (opt == "-i") config.io_schedulers = std::stoul(val);
        else {
            std::cerr << "Unknown option: " << opt << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    std::cout << "=== RPC Stream Benchmark Client ===\n";
    std::cout << "Target: " << config.host << ":" << config.port << "\n";
    std::cout << "Connections: " << config.connections << "\n";
    std::cout << "Payload size: " << config.payload_size << " bytes\n";
    std::cout << "Frames per stream: " << config.frames_per_stream << "\n";
    std::cout << "Frame window: " << std::max<size_t>(1, std::min(config.frame_window, config.frames_per_stream)) << "\n";
    std::cout << "Duration: " << config.duration_sec << " seconds\n";
    const size_t resolved_io_schedulers = resolveIoSchedulerCount(config.io_schedulers);
    std::cout << "IO Schedulers: "
              << (config.io_schedulers == 0
                      ? "auto (" + std::to_string(resolved_io_schedulers) + ")"
                      : std::to_string(resolved_io_schedulers))
              << "\n\n";

    g_stream_latencies.reserve(config.connections * std::max<size_t>(config.duration_sec, size_t{1}) * 512);

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(resolved_io_schedulers).computeSchedulerCount(1).build();
    runtime.start();

    std::cout << "Starting " << config.connections << " stream connections...\n";
    bool schedule_failed = false;
    for (size_t i = 0; i < config.connections; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (scheduler == nullptr ||
            !scheduleTask(scheduler, benchWorker(config, static_cast<uint32_t>(i)))) {
            schedule_failed = true;
            break;
        }
    }
    if (schedule_failed) {
        std::cerr << "Failed to schedule stream benchmark workers on IO scheduler(s)\n";
        g_running.store(false, std::memory_order_relaxed);
        runtime.stop();
        return 1;
    }

    std::cout << "Benchmark running...\n\n";

    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_streams = 0;
    uint64_t last_frames = 0;
    uint64_t last_bytes = 0;

    for (size_t sec = 0; sec < config.duration_sec && g_running.load(std::memory_order_relaxed); ++sec) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        uint64_t current_streams = g_total_streams.load(std::memory_order_relaxed);
        uint64_t current_frames = g_total_frames.load(std::memory_order_relaxed);
        uint64_t current_bytes = g_total_bytes.load(std::memory_order_relaxed);

        const uint64_t streams_per_sec = current_streams - last_streams;
        const uint64_t frames_per_sec = current_frames - last_frames;
        const double throughput_mb = static_cast<double>(current_bytes - last_bytes) / (1024.0 * 1024.0);

        std::cout << "[" << std::setw(3) << (sec + 1) << "s] "
                  << "Streams/s: " << std::setw(7) << streams_per_sec
                  << " | Frames/s: " << std::setw(9) << frames_per_sec
                  << " | Throughput: " << std::fixed << std::setprecision(2) << throughput_mb << " MB/s\n";

        last_streams = current_streams;
        last_frames = current_frames;
        last_bytes = current_bytes;
    }

    g_running.store(false, std::memory_order_relaxed);
    auto end_time = std::chrono::steady_clock::now();

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_workers_finished.load(std::memory_order_relaxed) < config.connections &&
           std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.stop();

    const double elapsed_sec =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;

    const uint64_t total_streams = g_total_streams.load(std::memory_order_relaxed);
    const uint64_t total_frames = g_total_frames.load(std::memory_order_relaxed);
    const uint64_t total_bytes = g_total_bytes.load(std::memory_order_relaxed);

    const double avg_streams_per_sec = elapsed_sec > 0 ? static_cast<double>(total_streams) / elapsed_sec : 0.0;
    const double avg_frames_per_sec = elapsed_sec > 0 ? static_cast<double>(total_frames) / elapsed_sec : 0.0;
    const double avg_throughput = elapsed_sec > 0
        ? (static_cast<double>(total_bytes) / (1024.0 * 1024.0)) / elapsed_sec
        : 0.0;

    std::cout << "\n=== Stream Benchmark Results ===\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << elapsed_sec << " seconds\n";
    std::cout << "Total Streams: " << total_streams << "\n";
    std::cout << "Total Frames: " << total_frames << "\n";
    std::cout << "Average Streams/s: " << std::fixed << std::setprecision(0) << avg_streams_per_sec << "\n";
    std::cout << "Average Frames/s: " << std::fixed << std::setprecision(0) << avg_frames_per_sec << "\n";
    std::cout << "Average Throughput: " << std::fixed << std::setprecision(2) << avg_throughput << " MB/s\n";

    if (!g_stream_latencies.empty()) {
        std::sort(g_stream_latencies.begin(), g_stream_latencies.end());
        const size_t n = g_stream_latencies.size();

        const auto p50 = g_stream_latencies[n * 50 / 100];
        const auto p90 = g_stream_latencies[n * 90 / 100];
        const auto p95 = g_stream_latencies[n * 95 / 100];
        const auto p99 = g_stream_latencies[n * 99 / 100];
        const auto p999 = g_stream_latencies[std::min(n * 999 / 1000, n - 1)];
        const auto max_lat = g_stream_latencies[n - 1];

        uint64_t sum = 0;
        for (auto lat : g_stream_latencies) {
            sum += lat;
        }
        const double avg_lat = static_cast<double>(sum) / n;

        std::cout << "\n=== Stream Latency (microseconds) ===\n";
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
