/**
 * @file b5_udp.cc
 * @brief 用途：作为 UDP 压测客户端，发起多客户端报文发送并统计回包结果。
 * 关键覆盖点：多客户端并发、报文大小与时长参数、收发计数与吞吐统计。
 * 通过条件：客户端完成设定压测周期并输出统计结果，进程干净退出。
 */

#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>
#include <vector>
#include <csignal>
#include <galay/cpp/galay-kernel/async/udp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;

// 全局统计
std::atomic<uint64_t> g_total_sent{0};
std::atomic<uint64_t> g_total_received{0};
std::atomic<uint64_t> g_total_bytes_sent{0};
std::atomic<uint64_t> g_total_bytes_received{0};
std::atomic<bool> g_running{true};
std::atomic<int> g_active_clients{0};

// 配置参数（可通过命令行参数修改）
int g_num_clients = 100;           // 并发客户端数量
int g_messages_per_client = 1000;  // 每个客户端发送的消息数
int g_message_size = 256;          // 消息大小（字节）
int g_test_duration_sec = 5;       // 测试持续时间（秒）
std::string g_server_host = "127.0.0.1";  // 服务器地址
int g_server_port = 9090;          // 服务器端口

// 全局调度器指针
IOScheduler* g_scheduler = nullptr;

// 信号处理函数
void signalHandler(int signum) {
    LogInfo("\nReceived signal {}, shutting down clients...", signum);
    g_running.store(false, std::memory_order_relaxed);
}

// UDP客户端协程 - 流水线模式
Task<void> udpBenchmarkClient(int client_id) {
    g_active_clients.fetch_add(1, std::memory_order_relaxed);

    UdpSocket socket;
    socket.option().handleNonBlock();

    // 设置发送缓冲区大小
    int send_buf_size = 2 * 1024 * 1024; // 2MB
    setsockopt(socket.handle().fd, SOL_SOCKET, SO_SNDBUF,
               &send_buf_size, sizeof(send_buf_size));

    Host serverHost(IPType::IPV4, g_server_host, g_server_port);

    // 准备测试数据
    std::vector<char> message(g_message_size);
    snprintf(message.data(), g_message_size, "Client-%d-Message", client_id);

    char recv_buffer[65536];
    uint64_t local_sent = 0;
    uint64_t local_received = 0;

    // 流水线模式：先发送一批，再接收一批
    constexpr int PIPELINE_SIZE = 10;  // 流水线深度

    for (int batch = 0; batch < g_messages_per_client / PIPELINE_SIZE && g_running.load(std::memory_order_relaxed); ++batch) {
        // 批量发送
        for (int i = 0; i < PIPELINE_SIZE; ++i) {
            auto sendResult = co_await socket.sendto(message.data(), g_message_size, serverHost);
            if (sendResult) {
                local_sent++;
            }
        }

        // 批量接收
        // UDP 存在丢包，使用超时避免单个丢包导致客户端协程永久阻塞。
        for (int i = 0; i < PIPELINE_SIZE; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                break;
            }
            Host from;
            auto recvResult = co_await socket.recvfrom(recv_buffer, sizeof(recv_buffer), &from)
                                    .timeout(std::chrono::milliseconds(50));
            if (recvResult) {
                local_received++;
            }
        }
    }

    g_total_sent.fetch_add(local_sent, std::memory_order_relaxed);
    g_total_received.fetch_add(local_received, std::memory_order_relaxed);
    g_total_bytes_sent.fetch_add(local_sent * g_message_size, std::memory_order_relaxed);
    g_total_bytes_received.fetch_add(local_received * g_message_size, std::memory_order_relaxed);

    co_await socket.close();
    g_active_clients.fetch_sub(1, std::memory_order_relaxed);
    co_return;
}

void printBenchmarkResults(std::chrono::steady_clock::time_point start_time) {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;

    uint64_t total_sent = g_total_sent.load();
    uint64_t total_received = g_total_received.load();
    uint64_t total_bytes_sent = g_total_bytes_sent.load();
    uint64_t total_bytes_received = g_total_bytes_received.load();

    LogInfo("\n========== UDP Benchmark Results ==========");
    LogInfo("Test Duration: {:.2f} seconds", duration_sec);
    LogInfo("Server: {}:{}", g_server_host, g_server_port);
    LogInfo("Concurrent Clients: {}", g_num_clients);
    LogInfo("Messages per Client: {}", g_messages_per_client);
    LogInfo("Message Size: {} bytes", g_message_size);
    LogInfo("");
    LogInfo("Total Packets Sent: {}", total_sent);
    LogInfo("Total Packets Received: {}", total_received);
    LogInfo("Packet Loss Rate: {:.2f}%",
            total_sent > 0 ? (1.0 - (double)total_received / total_sent) * 100.0 : 0.0);
    LogInfo("");
    LogInfo("Total Data Sent: {:.2f} MB", total_bytes_sent / 1024.0 / 1024.0);
    LogInfo("Total Data Received: {:.2f} MB", total_bytes_received / 1024.0 / 1024.0);
    LogInfo("");
    LogInfo("Average Throughput:");
    LogInfo("  Sent: {:.2f} pkt/s ({:.2f} MB/s)",
            total_sent / duration_sec,
            total_bytes_sent / duration_sec / 1024.0 / 1024.0);
    LogInfo("  Received: {:.2f} pkt/s ({:.2f} MB/s)",
            total_received / duration_sec,
            total_bytes_received / duration_sec / 1024.0 / 1024.0);
    LogInfo("===========================================\n");
}

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  -h, --host <host>       Server host (default: 127.0.0.1)\n"
              << "  -p, --port <port>       Server port (default: 9090)\n"
              << "  -c, --clients <num>     Number of concurrent clients (default: 100)\n"
              << "  -m, --messages <num>    Messages per client (default: 1000)\n"
              << "  -s, --size <bytes>      Message size in bytes (default: 256)\n"
              << "  -d, --duration <sec>    Test duration in seconds (default: 5)\n"
              << "  --help                  Show this help message\n";
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
            g_server_host = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            g_server_port = std::atoi(argv[++i]);
        } else if ((arg == "-c" || arg == "--clients") && i + 1 < argc) {
            g_num_clients = std::atoi(argv[++i]);
        } else if ((arg == "-m" || arg == "--messages") && i + 1 < argc) {
            g_messages_per_client = std::atoi(argv[++i]);
        } else if ((arg == "-s" || arg == "--size") && i + 1 < argc) {
            g_message_size = std::atoi(argv[++i]);
        } else if ((arg == "-d" || arg == "--duration") && i + 1 < argc) {
            g_test_duration_sec = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    LogInfo("UDP Benchmark Client");
    LogInfo("Configuration: {} clients, {} messages/client, {} bytes/message",
            g_num_clients, g_messages_per_client, g_message_size);
    LogInfo("Target Server: {}:{}", g_server_host, g_server_port);

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

#ifdef USE_KQUEUE
    LogInfo("Using KqueueScheduler (macOS)");
    KqueueScheduler scheduler;
#elif defined(USE_EPOLL)
    LogInfo("Using EpollScheduler (Linux)");
    EpollScheduler scheduler;
#elif defined(USE_IOURING)
    LogInfo("Using IOUringScheduler (Linux io_uring)");
    IOUringScheduler scheduler;
#else
    LogError("This benchmark requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    g_scheduler = &scheduler;
    scheduler.start();
    LogInfo("Scheduler started");

    auto start_time = std::chrono::steady_clock::now();

    // 启动多个客户端
    LogInfo("Starting {} clients...", g_num_clients);
    for (int i = 0; i < g_num_clients; ++i) {
        scheduleTask(scheduler, udpBenchmarkClient(i));
    }

    // 运行测试
    LogInfo("Benchmark running for {} seconds...", g_test_duration_sec);
    std::this_thread::sleep_for(std::chrono::seconds(g_test_duration_sec));

    // 停止测试
    g_running.store(false, std::memory_order_relaxed);

    // 等待所有客户端完成
    LogInfo("Waiting for clients to finish...");
    while (g_active_clients.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    scheduler.stop();
    LogInfo("Scheduler stopped");

    // 打印结果
    printBenchmarkResults(start_time);

    return 0;
}
