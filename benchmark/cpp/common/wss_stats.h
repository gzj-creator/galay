#ifndef GALAY_HTTP_BENCHMARK_WSS_BENCH_STATS_H
#define GALAY_HTTP_BENCHMARK_WSS_BENCH_STATS_H

#include <atomic>
#include <cstdint>
#include <limits>

namespace galay::benchmark {

struct WssBenchGlobalStats {
    std::atomic<uint64_t> total_connections{0};
    std::atomic<uint64_t> successful_connections{0};
    std::atomic<uint64_t> failed_connections{0};
    std::atomic<uint64_t> total_messages_sent{0};
    std::atomic<uint64_t> total_messages_received{0};
    std::atomic<uint64_t> total_bytes_sent{0};
    std::atomic<uint64_t> total_bytes_received{0};
    std::atomic<uint64_t> latency_sum_us{0};
    std::atomic<uint64_t> latency_count{0};
    std::atomic<uint32_t> latency_min_us{std::numeric_limits<uint32_t>::max()};
    std::atomic<uint32_t> latency_max_us{0};
};

struct WssClientStatsBatch {
    uint64_t total_connections = 0;
    uint64_t successful_connections = 0;
    uint64_t failed_connections = 0;
    uint64_t messages_sent = 0;
    uint64_t messages_received = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t latency_sum_us = 0;
    uint64_t latency_count = 0;
    uint32_t latency_min_us = std::numeric_limits<uint32_t>::max();
    uint32_t latency_max_us = 0;

    void noteMessageSent(size_t bytes) {
        ++messages_sent;
        bytes_sent += bytes;
    }

    void noteMessageReceived(size_t bytes) {
        ++messages_received;
        bytes_received += bytes;
    }

    void noteLatency(uint32_t latency_us) {
        latency_sum_us += latency_us;
        ++latency_count;
        if (latency_us < latency_min_us) {
            latency_min_us = latency_us;
        }
        if (latency_us > latency_max_us) {
            latency_max_us = latency_us;
        }
    }

    void mergeInto(WssBenchGlobalStats& global) const {
        global.total_connections.fetch_add(total_connections, std::memory_order_relaxed);
        global.successful_connections.fetch_add(successful_connections, std::memory_order_relaxed);
        global.failed_connections.fetch_add(failed_connections, std::memory_order_relaxed);
        global.total_messages_sent.fetch_add(messages_sent, std::memory_order_relaxed);
        global.total_messages_received.fetch_add(messages_received, std::memory_order_relaxed);
        global.total_bytes_sent.fetch_add(bytes_sent, std::memory_order_relaxed);
        global.total_bytes_received.fetch_add(bytes_received, std::memory_order_relaxed);
        global.latency_sum_us.fetch_add(latency_sum_us, std::memory_order_relaxed);
        global.latency_count.fetch_add(latency_count, std::memory_order_relaxed);

        if (latency_count == 0) {
            return;
        }

        uint32_t old_min = global.latency_min_us.load(std::memory_order_relaxed);
        while (latency_min_us < old_min &&
               !global.latency_min_us.compare_exchange_weak(
                   old_min,
                   latency_min_us,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {}

        uint32_t old_max = global.latency_max_us.load(std::memory_order_relaxed);
        while (latency_max_us > old_max &&
               !global.latency_max_us.compare_exchange_weak(
                   old_max,
                   latency_max_us,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {}
    }
};

} // namespace galay::benchmark

#endif
