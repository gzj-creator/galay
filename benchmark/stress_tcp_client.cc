/**
 * @file stress_tcp_client.cc
 * @brief TCP客户端压力测试
 * @details 多线程并发TCP客户端压测工具，支持自定义并发数和请求数
 */

#include "galay/kernel/client/TcpClient.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Buffer.h"
#include "galay/utils/Thread.h"
#include <atomic>
#include <chrono>
#include <vector>
#include <iomanip>
#include <random>

using namespace galay;

// 压测配置
struct BenchmarkConfig {
    std::string server_ip = "127.0.0.1";
    uint32_t server_port = 8070;
    int client_count = 100;              // 并发客户端数
    int requests_per_client = 1000;      // 每客户端请求数
    int message_size = 1024;             // 消息大小（字节）
    bool keep_alive = true;              // 是否保持连接
};

// 统计数据
std::atomic<uint64_t> total_requests{0};
std::atomic<uint64_t> total_responses{0};
std::atomic<uint64_t> total_errors{0};
std::atomic<uint64_t> total_bytes_sent{0};
std::atomic<uint64_t> total_bytes_recv{0};
std::atomic<uint64_t> total_latency_us{0};  // 微秒

std::mutex mtx;
std::condition_variable cond;
std::atomic<int> running_clients{0};

/**
 * @brief 生成测试消息
 */
std::string generateMessage(int size) {
    static const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string result;
    result.reserve(size);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);
    
    for (int i = 0; i < size; ++i) {
        result += charset[dis(gen)];
    }
    return result;
}

/**
 * @brief 单个客户端压测协程
 */
Coroutine<nil> clientBenchmark(Runtime& runtime, const BenchmarkConfig& config, int client_id)
{
    try {
        auto handle = runtime.getCoSchedulerHandle(0).value();
        TcpClient client(handle);
        
        // 连接服务器
        auto res1 = co_await client.connect({config.server_ip, config.server_port});
        if (!res1) {
            std::cerr << "客户端 " << client_id << " 连接失败: " << res1.error().message() << std::endl;
            total_errors++;
            running_clients--;
            if (running_clients == 0) {
                cond.notify_one();
            }
            co_return nil();
        }
        
        // 准备消息和缓冲区
        std::string message = generateMessage(config.message_size);
        Buffer buffer(config.message_size + 100);
        
        // 发送请求
        for (int i = 0; i < config.requests_per_client; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            
            // 发送数据
            auto res2 = co_await client.send(Bytes::fromString(message));
            if (!res2) {
                std::cerr << "客户端 " << client_id << " 发送失败: " << res2.error().message() << std::endl;
                total_errors++;
                break;
            }
            total_requests++;
            total_bytes_sent += config.message_size;
            
            // 接收响应
            auto res3 = co_await client.recv(buffer.data(), buffer.capacity());
            if (!res3) {
                std::cerr << "客户端 " << client_id << " 接收失败: " << res3.error().message() << std::endl;
                total_errors++;
                break;
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            total_latency_us += latency;
            
            total_responses++;
            total_bytes_recv += res3.value().size();
            
            // 定期打印进度
            if (client_id == 0 && (i + 1) % 100 == 0) {
                std::cout << "客户端 0 完成 " << (i + 1) << " 个请求" << std::endl;
            }
        }
        
        // 关闭连接
        co_await client.close();
        
        if (client_id % 10 == 0) {
            std::cout << "客户端 " << client_id << " 完成测试" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "客户端 " << client_id << " 异常: " << e.what() << std::endl;
        total_errors++;
    }
    
    running_clients--;
    if (running_clients == 0) {
        cond.notify_one();
    }
    
    co_return nil();
}

/**
 * @brief 打印压测统计
 */
void printBenchmarkResults(const BenchmarkConfig& config, double elapsed_seconds) {
    uint64_t requests = total_requests.load();
    uint64_t responses = total_responses.load();
    uint64_t errors = total_errors.load();
    uint64_t bytes_sent = total_bytes_sent.load();
    uint64_t bytes_recv = total_bytes_recv.load();
    uint64_t latency_us = total_latency_us.load();
    
    std::cout << "\n========== 压力测试结果 ==========" << std::endl;
    std::cout << "测试配置:" << std::endl;
    std::cout << "  服务器: " << config.server_ip << ":" << config.server_port << std::endl;
    std::cout << "  并发客户端: " << config.client_count << std::endl;
    std::cout << "  每客户端请求: " << config.requests_per_client << std::endl;
    std::cout << "  消息大小: " << config.message_size << " 字节" << std::endl;
    std::cout << "\n性能指标:" << std::endl;
    std::cout << "  总请求数: " << requests << std::endl;
    std::cout << "  成功响应: " << responses << std::endl;
    std::cout << "  失败数: " << errors << std::endl;
    std::cout << "  成功率: " << std::fixed << std::setprecision(2) 
              << (responses * 100.0 / (requests > 0 ? requests : 1)) << "%" << std::endl;
    std::cout << "  测试时间: " << std::fixed << std::setprecision(2) 
              << elapsed_seconds << " 秒" << std::endl;
    std::cout << "\n吞吐量:" << std::endl;
    std::cout << "  QPS: " << std::fixed << std::setprecision(0)
              << (responses / elapsed_seconds) << " 请求/秒" << std::endl;
    std::cout << "  发送: " << std::fixed << std::setprecision(2)
              << (bytes_sent / elapsed_seconds / 1024.0 / 1024.0) << " MB/s" << std::endl;
    std::cout << "  接收: " << std::fixed << std::setprecision(2)
              << (bytes_recv / elapsed_seconds / 1024.0 / 1024.0) << " MB/s" << std::endl;
    std::cout << "\n延迟:" << std::endl;
    std::cout << "  平均延迟: " << std::fixed << std::setprecision(2)
              << (latency_us / (double)(responses > 0 ? responses : 1) / 1000.0) << " ms" << std::endl;
    std::cout << "================================\n" << std::endl;
}

int main(int argc, char* argv[])
{
    BenchmarkConfig config;
    
    // 解析命令行参数
    if (argc >= 2) {
        config.server_ip = argv[1];
    }
    if (argc >= 3) {
        config.server_port = std::stoi(argv[2]);
    }
    if (argc >= 4) {
        config.client_count = std::stoi(argv[3]);
    }
    if (argc >= 5) {
        config.requests_per_client = std::stoi(argv[4]);
    }
    if (argc >= 6) {
        config.message_size = std::stoi(argv[5]);
    }
    
    std::cout << "========== TCP客户端压力测试 ==========" << std::endl;
    std::cout << "服务器: " << config.server_ip << ":" << config.server_port << std::endl;
    std::cout << "并发客户端: " << config.client_count << std::endl;
    std::cout << "每客户端请求: " << config.requests_per_client << std::endl;
    std::cout << "消息大小: " << config.message_size << " 字节" << std::endl;
    std::cout << "总请求数: " << (config.client_count * config.requests_per_client) << std::endl;
    std::cout << "======================================\n" << std::endl;
    
    // 等待用户确认
    std::cout << "按回车开始测试..." << std::endl;
    std::cin.get();
    
    // 创建运行时和线程池
    std::vector<std::unique_ptr<Runtime>> runtimes;
    int runtime_count = std::min(config.client_count, (int)std::thread::hardware_concurrency());
    
    for (int i = 0; i < runtime_count; ++i) {
        RuntimeBuilder builder;
        runtimes.push_back(std::make_unique<Runtime>(builder.build()));
        runtimes.back()->start();
    }
    
    std::cout << "使用 " << runtime_count << " 个运行时" << std::endl;
    
    // 发送start命令到服务器
    std::cout << "\n发送start命令到服务器..." << std::endl;
    Runtime& ctrl_runtime = *runtimes[0];
    std::atomic<bool> start_cmd_done{false};
    ctrl_runtime.schedule([&ctrl_runtime, &config, &start_cmd_done]() -> Coroutine<nil> {
        try {
            auto handle = ctrl_runtime.getCoSchedulerHandle(0).value();
            TcpClient ctrl_client(handle);
            auto connect_res = co_await ctrl_client.connect({config.server_ip, config.server_port});
            if (connect_res) {
                auto cmd = Bytes::fromString(std::string("start"));
                auto send_res = co_await ctrl_client.send(std::move(cmd));
                if (send_res) {
                    Buffer buf(100);
                    auto recv_res = co_await ctrl_client.recv(buf.data(), buf.capacity());
                    if (recv_res) {
                        std::cout << "✓ 服务器确认: " << recv_res.value().toString() << std::endl;
                    }
                }
                co_await ctrl_client.close();
            }
        } catch (...) {
            std::cerr << "发送start命令异常" << std::endl;
        }
        start_cmd_done = true;
        co_return nil();
    }());
    
    // 等待start命令完成
    while (!start_cmd_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    std::cout << "开始压力测试...\n" << std::endl;
    
    // 记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();
    running_clients = config.client_count;
    
    // 启动所有客户端协程
    for (int i = 0; i < config.client_count; ++i) {
        Runtime& runtime = *runtimes[i % runtime_count];
        runtime.schedule(clientBenchmark(runtime, config, i));
    }
    
    // 等待所有客户端完成
    {
        std::unique_lock<std::mutex> lock(mtx);
        cond.wait(lock, []{ return running_clients == 0; });
    }
    
    // 记录结束时间
    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    
    std::cout << "\n所有客户端测试完成！" << std::endl;
    
    // 发送finish命令到服务器
    std::cout << "发送finish命令到服务器..." << std::endl;
    std::atomic<bool> finish_cmd_done{false};
    ctrl_runtime.schedule([&ctrl_runtime, &config, &finish_cmd_done]() -> Coroutine<nil> {
        try {
            auto handle = ctrl_runtime.getCoSchedulerHandle(0).value();
            TcpClient ctrl_client(handle);
            auto connect_res = co_await ctrl_client.connect({config.server_ip, config.server_port});
            if (connect_res) {
                auto cmd = Bytes::fromString(std::string("finish"));
                auto send_res = co_await ctrl_client.send(std::move(cmd));
                if (send_res) {
                    Buffer buf(100);
                    auto recv_res = co_await ctrl_client.recv(buf.data(), buf.capacity());
                    if (recv_res) {
                        std::cout << "✓ 服务器确认: " << recv_res.value().toString() << std::endl;
                    }
                }
                co_await ctrl_client.close();
            }
        } catch (...) {
            std::cerr << "发送finish命令异常" << std::endl;
        }
        finish_cmd_done = true;
        co_return nil();
    }());
    
    // 等待finish命令完成
    while (!finish_cmd_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // 打印结果
    printBenchmarkResults(config, elapsed);
    
    // 停止所有运行时
    for (auto& runtime : runtimes) {
        runtime->stop();
    }
    
    return 0;
}

