/**
 * @file b2_http.cc
 * @brief HTTP 客户端压力测试（持续压测模式）
 * @details 类似 wrk 的持续压测，测试服务器的真实 QPS
 */

#include <galay/cpp/galay-http/client/http_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <map>

using namespace galay::http;
using namespace galay::kernel;

std::atomic<int64_t> g_success{0};
std::atomic<int64_t> g_fail{0};
std::atomic<int64_t> g_bytes_sent{0};
std::atomic<int64_t> g_bytes_recv{0};
std::atomic<int64_t> g_request_time_us{0};
std::atomic<int> g_active_connections{0};

/**
 * @brief 持续压测工作协程（类似 wrk）
 */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline))
#endif
Task<void> continuousWorker(int worker_id, const std::string& host, int port, const std::string& path,
                            std::chrono::steady_clock::time_point end_time,
                            std::atomic<bool>& stop_flag) {
    (void)worker_id;
    g_active_connections++;
    auto client = HttpClientBuilder().build();

    std::string url = "http://" + host + ":" + std::to_string(port) + path;

    auto connect_result = co_await client.connect(url);
    if (!connect_result) {
        g_fail++;
        g_active_connections--;
        co_return;
    }

    auto socket_result = client.socket();
    if (!socket_result) {
        g_fail++;
        g_active_connections--;
        co_return;
    }
    HttpSession session(socket_result.value().get());
    const std::string request_path = client.url().path;
    std::map<std::string, std::string> headers{
        {"Host", host},
        {"Connection", "keep-alive"}
    };

    // 持续发送请求直到时间到或收到停止信号
    while (!stop_flag.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < end_time) {
        auto request_start = std::chrono::steady_clock::now();
        auto result = co_await session.get(request_path, headers);
        if (!result) {
            g_fail++;
            break;
        }

        auto response_opt = result.value();
        if (!response_opt.has_value()) {
            continue;
        }

        auto& response = response_opt.value();
        auto request_end = std::chrono::steady_clock::now();
        g_request_time_us += std::chrono::duration_cast<std::chrono::microseconds>(request_end - request_start).count();

        if (static_cast<int>(response.header().code()) == 200) {
            g_success++;
            g_bytes_recv += response.getBodyStr().size();
            g_bytes_sent += 100;  // 估算请求大小
        } else {
            g_fail++;
        }
    }

    // GCC13 协程在复杂析构路径上存在已知 ICE，这里依赖析构关闭 socket。

    g_active_connections--;
    co_return;
}

/**
 * @brief 运行持续压测（类似 wrk）
 */
void runContinuousBenchmark(Runtime& rt, int connections, int duration_sec,
                            const std::string& host, int port, const std::string& path) {
    g_success = 0;
    g_fail = 0;
    g_bytes_sent = 0;
    g_bytes_recv = 0;
    g_request_time_us = 0;
    g_active_connections = 0;

    std::atomic<bool> stop_flag{false};

    std::cout << "\n==========================================\n";
    std::cout << "持续压测模式 (类似 wrk)\n";
    std::cout << "==========================================\n";
    std::cout << "连接数: " << connections << "\n";
    std::cout << "持续时间: " << duration_sec << " 秒\n";
    std::cout << "目标: http://" << host << ":" << port << path << "\n";
    std::cout << "==========================================\n\n";

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    // 启动所有工作协程
    std::cout << "启动 " << connections << " 个连接...\n";
    for (int i = 0; i < connections; i++) {
        auto* scheduler = rt.getNextIOScheduler();
        if (scheduler) {
            scheduleTask(scheduler, continuousWorker(i, host, port, path, end_time, stop_flag));
        }
    }

    // 等待连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "活跃连接数: " << g_active_connections.load() << "\n";
    std::cout << "开始压测...\n\n";

    // 等待测试完成
    std::this_thread::sleep_until(end_time);
    stop_flag.store(true);

    // 等待所有协程完成
    std::cout << "等待连接关闭...\n";
    while (g_active_connections.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double duration_s = duration_ms / 1000.0;

    int64_t total_requests = g_success.load() + g_fail.load();
    double qps = (duration_s > 0) ? (total_requests / duration_s) : 0;
    double success_rate = (total_requests > 0) ? (g_success.load() * 100.0 / total_requests) : 0;
    double throughput_mb = (g_bytes_recv.load() + g_bytes_sent.load()) / 1024.0 / 1024.0;
    double throughput_mbps = (duration_s > 0) ? (throughput_mb / duration_s) : 0;
    double avg_request_us = (g_success.load() > 0) ? (g_request_time_us.load() * 1.0 / g_success.load()) : 0;

    std::cout << "\n==========================================\n";
    std::cout << "压测结果\n";
    std::cout << "==========================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "持续时间: " << duration_s << " 秒\n";
    std::cout << "总请求数: " << total_requests << "\n";
    std::cout << "成功: " << g_success << " (" << success_rate << "%)\n";
    std::cout << "失败: " << g_fail << "\n";
    std::cout << "\n性能指标:\n";
    std::cout << "  QPS: " << qps << " 请求/秒\n";
    std::cout << "  吞吐量: " << throughput_mbps << " MB/秒\n";
    std::cout << "  平均延迟: " << (avg_request_us / 1000.0) << " ms\n";
    std::cout << "==========================================\n";
}

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 8080;
    std::string path = "/";
    int connections = 100;
    int duration = 10;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);
    if (argc > 3) connections = std::atoi(argv[3]);
    if (argc > 4) duration = std::atoi(argv[4]);
    if (argc > 5) path = argv[5];

    std::cout << "==========================================\n";
    std::cout << "HTTP Client Benchmark\n";
    std::cout << "==========================================\n";
    std::cout << "用法: " << argv[0] << " [host] [port] [connections] [duration] [path]\n";
    std::cout << "默认: localhost 8080 100 10 /\n";
    std::cout << "==========================================\n";

    Runtime rt = RuntimeBuilder().ioSchedulerCount(4).computeSchedulerCount(0).build();
    rt.start();

    // 运行持续压测
    runContinuousBenchmark(rt, connections, duration, host, port, path);

    rt.stop();

    return 0;
}
