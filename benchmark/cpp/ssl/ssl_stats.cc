#include "ssl_stats.h"
#include <atomic>

namespace galay::ssl::bench
{

namespace {
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_epoch{1};
std::atomic<uint64_t> g_send_ops{0};
std::atomic<uint64_t> g_send_plain_bytes{0};
std::atomic<uint64_t> g_recv_ops{0};
std::atomic<uint64_t> g_recv_plain_bytes{0};
std::atomic<uint64_t> g_recv_chunks{0};
constexpr uint32_t kFlushThreshold = 64;

void reset_stats()
{
    g_send_ops.store(0, std::memory_order_relaxed);
    g_send_plain_bytes.store(0, std::memory_order_relaxed);
    g_recv_ops.store(0, std::memory_order_relaxed);
    g_recv_plain_bytes.store(0, std::memory_order_relaxed);
    g_recv_chunks.store(0, std::memory_order_relaxed);
}

struct LocalStats {
    uint64_t epoch = 0;
    uint32_t pending = 0;
    uint64_t send_ops = 0;
    uint64_t send_plain_bytes = 0;
    uint64_t recv_ops = 0;
    uint64_t recv_plain_bytes = 0;
    uint64_t recv_chunks = 0;
};

void flush_local(LocalStats& local)
{
    if (local.pending == 0) {
        return;
    }
    g_send_ops.fetch_add(local.send_ops, std::memory_order_relaxed);
    g_send_plain_bytes.fetch_add(local.send_plain_bytes, std::memory_order_relaxed);
    g_recv_ops.fetch_add(local.recv_ops, std::memory_order_relaxed);
    g_recv_plain_bytes.fetch_add(local.recv_plain_bytes, std::memory_order_relaxed);
    g_recv_chunks.fetch_add(local.recv_chunks, std::memory_order_relaxed);

    local.pending = 0;
    local.send_ops = 0;
    local.send_plain_bytes = 0;
    local.recv_ops = 0;
    local.recv_plain_bytes = 0;
    local.recv_chunks = 0;
}

struct ThreadStats {
    LocalStats local;

    ~ThreadStats()
    {
        if (local.pending == 0) {
            return;
        }
        if (local.epoch != g_epoch.load(std::memory_order_relaxed)) {
            return;
        }
        flush_local(local);
    }
};

thread_local ThreadStats g_threadStats;

bool prepare_local(LocalStats& local)
{
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    const uint64_t epoch = g_epoch.load(std::memory_order_relaxed);
    if (local.epoch != epoch) {
        local = LocalStats{};
        local.epoch = epoch;
    }
    return true;
}

void flush_current_thread()
{
    auto& local = g_threadStats.local;
    if (local.pending == 0) {
        return;
    }
    if (local.epoch != g_epoch.load(std::memory_order_relaxed)) {
        local = LocalStats{};
        return;
    }
    flush_local(local);
}
} // namespace

void ssl_stats_set_enabled(bool enabled)
{
    g_enabled.store(false, std::memory_order_relaxed);
    g_epoch.fetch_add(1, std::memory_order_relaxed);
    reset_stats();
    if (!enabled) {
        return;
    }
    g_enabled.store(true, std::memory_order_relaxed);
}

bool ssl_stats_enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

SslIoStats ssl_stats_snapshot()
{
    if (!ssl_stats_enabled()) {
        return {};
    }
    flush_current_thread();

    SslIoStats stats;
    stats.send_ops = g_send_ops.load(std::memory_order_relaxed);
    stats.send_plain_bytes = g_send_plain_bytes.load(std::memory_order_relaxed);
    stats.recv_ops = g_recv_ops.load(std::memory_order_relaxed);
    stats.recv_plain_bytes = g_recv_plain_bytes.load(std::memory_order_relaxed);
    stats.recv_chunks = g_recv_chunks.load(std::memory_order_relaxed);
    return stats;
}

void ssl_stats_add_send(size_t bytes)
{
    auto& local = g_threadStats.local;
    if (!prepare_local(local)) {
        return;
    }
    local.send_ops += 1;
    local.send_plain_bytes += static_cast<uint64_t>(bytes);
    local.pending += 2;
    if (local.pending >= kFlushThreshold) {
        flush_local(local);
    }
}

void ssl_stats_add_recv(size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    auto& local = g_threadStats.local;
    if (!prepare_local(local)) {
        return;
    }
    local.recv_ops += 1;
    local.recv_plain_bytes += static_cast<uint64_t>(bytes);
    local.recv_chunks += 1;
    local.pending += 3;
    if (local.pending >= kFlushThreshold) {
        flush_local(local);
    }
}

} // namespace galay::ssl::bench
