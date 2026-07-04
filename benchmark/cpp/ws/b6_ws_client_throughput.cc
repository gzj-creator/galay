/**
 * @file bench_websocket_client.cc
 * @brief WebSocket 客户端压测程序
 */

#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <thread>
#include <cstdlib>
#include <string_view>
#include <galay/cpp/galay-kernel/concurrency/async_mutex.h>
#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-ws/client/ws_client.h>
#include <galay/cpp/galay-ws/kernel/writer_cfg.h>
#include "benchmark/cpp/ws/ws_benchmark_args.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::http;
using namespace galay::websocket;
using namespace galay::kernel;

// 统计数据
std::atomic<uint64_t> g_total_connections{0};
std::atomic<uint64_t> g_successful_connections{0};
std::atomic<uint64_t> g_failed_connections{0};
std::atomic<uint64_t> g_total_messages_sent{0};
std::atomic<uint64_t> g_total_messages_received{0};
std::atomic<uint64_t> g_total_bytes_sent{0};
std::atomic<uint64_t> g_total_bytes_received{0};
std::atomic<bool> g_stop{false};
std::atomic<int> g_active_clients{0};

// 延迟统计 - 使用原子变量
std::atomic<uint64_t> g_latency_sum_us{0};  // 总延迟（微秒）
std::atomic<uint64_t> g_latency_count{0};   // 延迟样本数量
std::atomic<uint32_t> g_latency_min_us{UINT32_MAX};  // 最小延迟
std::atomic<uint32_t> g_latency_max_us{0};  // 最大延迟

struct ClientGuard {
    explicit ClientGuard(std::atomic<int>& counter)
        : m_counter(&counter) {
        m_counter->fetch_add(1);
    }
    ~ClientGuard() {
        m_counter->fetch_sub(1);
    }
private:
    std::atomic<int>* m_counter;
};

static uint32_t toLatencyUs(std::chrono::steady_clock::duration duration) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    if (us < 0) {
        return 0;
    }
    return static_cast<uint32_t>(us);
}

/**
 * @brief 单个 WebSocket 客户端压测
 */
Task<void> benchmarkWebSocketClient(
    IOScheduler* scheduler,
    int client_id,
    const std::string& target_url,
    const std::string& message_payload,
    std::chrono::steady_clock::time_point end_time)
{
    (void)scheduler;
    ClientGuard guard(g_active_clients);
    g_total_connections.fetch_add(1);

    auto client = WsClientBuilder().build();

    auto connect_result = co_await client.connect(target_url);
    if (!connect_result) {
        g_failed_connections.fetch_add(1);
        co_return;
    }

    auto session_result = client.getSession(WsWriterSetting::byClient());
    if (!session_result) {
        g_failed_connections.fetch_add(1);
        co_return;
    }
    auto& session = *session_result.value();
    auto upgrader = session.upgrade();
    auto res = co_await upgrader();
    if(!res) {
        co_return;
    }
    if(!res.value()) {
        co_return;
    }


    g_successful_connections.fetch_add(1);

    auto ws_reader = session.getReader();
    auto ws_writer = session.getWriter();
    // 读取欢迎消息
    std::string welcome_msg;
    WsOpcode welcome_opcode;
    while(true) {
        auto welcome_result = co_await ws_reader.getMessage(welcome_msg, welcome_opcode);
        if(!welcome_result) {
            co_return;
        }
        if(welcome_result.value()) {
            g_total_messages_received.fetch_add(1);
            g_total_bytes_received.fetch_add(welcome_msg.size());
            break;
        }
    }

    WsFrame send_frame;
    send_frame.header.fin = true;
    send_frame.header.opcode = WsOpcode::Text;
    send_frame.header.mask = true;  // 客户端必须设置 mask
    send_frame.payload = message_payload;
    send_frame.header.payload_length = message_payload.size();

    std::string echo_msg;
    WsOpcode echo_opcode = WsOpcode::Text;

    // 发送和接收消息（固定时间压测）
    while (!g_stop.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < end_time) {
        auto round_start = std::chrono::steady_clock::now();
        while(true) {
            auto send_result = co_await ws_writer.sendFrame(send_frame);
            if (!send_result) {
                co_return;
            }
            if(send_result.value()) {
                g_total_messages_sent.fetch_add(1);
                g_total_bytes_sent.fetch_add(message_payload.size());
                break;
            }
        }
        // 读取回显消息
        echo_msg.clear();
        while(true) {
            auto echo_result = co_await ws_reader.getMessage(echo_msg, echo_opcode);
            if (!echo_result.has_value()) {
                // 连接错误，退出
                co_return;
            }
            if (echo_result.value()) {
                // 消息接收完成
                g_total_messages_received.fetch_add(1);
                g_total_bytes_received.fetch_add(echo_msg.size());

                // 记录延迟统计
                uint32_t latency_us = toLatencyUs(std::chrono::steady_clock::now() - round_start);
                g_latency_sum_us.fetch_add(latency_us);
                g_latency_count.fetch_add(1);

                // 更新最小值
                uint32_t old_min = g_latency_min_us.load();
                while (latency_us < old_min && !g_latency_min_us.compare_exchange_weak(old_min, latency_us));

                // 更新最大值
                uint32_t old_max = g_latency_max_us.load();
                while (latency_us > old_max && !g_latency_max_us.compare_exchange_weak(old_max, latency_us));

                break;
            }
            // 返回 false 表示消息未完成，继续循环等待
        }
        
    }

    // 关闭连接
    co_await client.close();
    co_return;
}

/**
 * @brief 打印统计信息
 */
void printStats(const std::chrono::steady_clock::time_point& start_time,
                const std::chrono::steady_clock::time_point& end_time) {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;

    std::cout << "\n========================================\n";
    std::cout << "WebSocket Benchmark Results\n";
    std::cout << "========================================\n";
    std::cout << "Duration: " << duration_sec << " seconds\n";
    std::cout << "\nConnections:\n";
    std::cout << "  Total:      " << g_total_connections.load() << "\n";
    std::cout << "  Successful: " << g_successful_connections.load() << "\n";
    std::cout << "  Failed:     " << g_failed_connections.load() << "\n";
    std::cout << "\nMessages:\n";
    std::cout << "  Sent:       " << g_total_messages_sent.load() << "\n";
    std::cout << "  Received:   " << g_total_messages_received.load() << "\n";
    std::cout << "\nData Transfer:\n";
    std::cout << "  Sent:       " << g_total_bytes_sent.load() << " bytes ("
              << (g_total_bytes_sent.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "  Received:   " << g_total_bytes_received.load() << " bytes ("
              << (g_total_bytes_received.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "\nThroughput:\n";
    std::cout << "  Messages/sec:  " << (g_total_messages_sent.load() / duration_sec) << "\n";
    std::cout << "  MB/sec (sent): " << (g_total_bytes_sent.load() / 1024.0 / 1024.0 / duration_sec) << "\n";
    std::cout << "  MB/sec (recv): " << (g_total_bytes_received.load() / 1024.0 / 1024.0 / duration_sec) << "\n";

    uint64_t latency_count = g_latency_count.load();
    if (latency_count > 0) {
        uint64_t latency_sum = g_latency_sum_us.load();
        uint32_t latency_min = g_latency_min_us.load();
        uint32_t latency_max = g_latency_max_us.load();
        double avg_ms = static_cast<double>(latency_sum) / latency_count / 1000.0;

        std::cout << "\nLatency (RTT):\n";
        std::cout << "  Count:    " << latency_count << "\n";
        std::cout << "  Min:      " << (latency_min / 1000.0) << " ms\n";
        std::cout << "  Avg:      " << avg_ms << " ms\n";
        std::cout << "  Max:      " << (latency_max / 1000.0) << " ms\n";
    }
    std::cout << "========================================\n";
}

int main(int argc, char* argv[]) {
    // 压测默认关闭日志，避免日志 IO 成为吞吐瓶颈。
    // 设置 GALAY_HTTP_BENCH_LOG=1 可开启文件日志。
    const char* bench_log = std::getenv("GALAY_HTTP_BENCH_LOG");
    if (bench_log != nullptr && std::string_view(bench_log) == "1") {
    } else {
    }

    // 解析命令行参数
    int num_clients = 10;
    double duration_sec = 10.0;
    int message_size = 1024;

    if (argc > 1) num_clients = std::stoi(argv[1]);
    if (argc > 2) duration_sec = std::stod(argv[2]);
    if (argc > 3) message_size = std::stoi(argv[3]);
    std::string target_url = galay::benchmark::ws::resolveBenchmarkClientUrl(argc, argv);

    std::cout << "========================================\n";
    std::cout << "WebSocket Client Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "Clients:            " << num_clients << "\n";
    std::cout << "Duration:           " << duration_sec << " seconds\n";
    std::cout << "Message size:       " << message_size << " bytes\n";
    std::cout << "Target URL:         " << target_url << "\n";
    std::cout << "========================================\n\n";

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    galay::kernel::Runtime rt;
    rt.start();

    // 生成测试消息
    std::string message_payload(message_size, 'A');

    // 记录开始时间
    auto start_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration<double>(duration_sec);
    auto end_time = start_time + std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);

    // 启动所有客户端，轮询分配到不同的 IO 调度器
    std::cout << "Starting " << num_clients << " clients...\n";
    for (int i = 0; i < num_clients; i++) {
        // 每个客户端使用不同的 IO 调度器，充分利用多核
        auto* scheduler = rt.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler for client " << i << "\n";
            return 1;
        }
        scheduleTask(scheduler, benchmarkWebSocketClient(scheduler, i, target_url, message_payload, end_time));
    }

    std::cout << "Running for " << duration_sec << " seconds...\n";
    std::this_thread::sleep_for(duration);
    g_stop.store(true);

    auto stop_time = std::chrono::steady_clock::now();
    rt.stop();

    // 打印统计信息
    printStats(start_time, stop_time);

    return 0;
#else
    std::cerr << "No scheduler defined. Please compile with -DUSE_KQUEUE, -DUSE_EPOLL, or -DUSE_IOURING\n";
    return 1;
#endif
}
