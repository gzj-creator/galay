#include <atomic>
#include <cstdint>
#include <iostream>

#include "benchmark/wss_stats.h"

int main() {
    using galay::benchmark::WssBenchGlobalStats;
    using galay::benchmark::WssClientStatsBatch;

    WssBenchGlobalStats global;

    WssClientStatsBatch first;
    first.total_connections = 1;
    first.successful_connections = 1;
    first.messages_sent = 2;
    first.messages_received = 3;
    first.bytes_sent = 2048;
    first.bytes_received = 3072;
    first.latency_sum_us = 30;
    first.latency_count = 2;
    first.latency_min_us = 10;
    first.latency_max_us = 20;
    first.mergeInto(global);

    WssClientStatsBatch second;
    second.total_connections = 1;
    second.failed_connections = 1;
    second.messages_sent = 4;
    second.messages_received = 4;
    second.bytes_sent = 4096;
    second.bytes_received = 4096;
    second.latency_sum_us = 80;
    second.latency_count = 2;
    second.latency_min_us = 30;
    second.latency_max_us = 50;
    second.mergeInto(global);

    if (global.total_connections.load() != 2 ||
        global.successful_connections.load() != 1 ||
        global.failed_connections.load() != 1) {
        std::cerr << "[T63] connection counters merge mismatch\n";
        return 1;
    }

    if (global.total_messages_sent.load() != 6 ||
        global.total_messages_received.load() != 7) {
        std::cerr << "[T63] message counters merge mismatch\n";
        return 1;
    }

    if (global.total_bytes_sent.load() != 6144 ||
        global.total_bytes_received.load() != 7168) {
        std::cerr << "[T63] byte counters merge mismatch\n";
        return 1;
    }

    if (global.latency_sum_us.load() != 110 ||
        global.latency_count.load() != 4) {
        std::cerr << "[T63] latency sum/count merge mismatch\n";
        return 1;
    }

    if (global.latency_min_us.load() != 10 ||
        global.latency_max_us.load() != 50) {
        std::cerr << "[T63] latency min/max merge mismatch\n";
        return 1;
    }

    std::cout << "T63-WssClientStatsBatch PASS\n";
    return 0;
}
