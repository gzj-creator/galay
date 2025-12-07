/**
 * @file stress_tcp_server.cc
 * @brief TCP服务器压力测试服务端
 * @details 基于Galay框架的高性能TCP压测服务器，支持echo和统计功能
 */

#include "galay/kernel/async/Bytes.h"
#include "galay/kernel/server/TcpServer.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/utils/BackTrace.h"
#include "galay/utils/SignalHandler.hpp"
#include "galay/common/Buffer.h"
#include <atomic>
#include <chrono>
#include <iomanip>

using namespace galay;

// 统计数据
std::atomic<uint64_t> total_connections{0};
std::atomic<uint64_t> total_requests{0};
std::atomic<uint64_t> total_bytes_received{0};
std::atomic<uint64_t> total_bytes_sent{0};
std::atomic<uint64_t> active_connections{0};

// 请求时间统计
std::atomic<bool> first_request_received{false};
std::atomic<int64_t> first_request_time_ns{0};  // 纳秒时间戳
std::atomic<int64_t> last_request_time_ns{0};   // 纳秒时间戳

/**
 * @brief 信号名称获取
 */
std::string getSignalName(int sig) {
    switch(sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGFPE:  return "SIGFPE (Floating Point Exception)";
        case SIGILL:  return "SIGILL (Illegal Instruction)";
        case SIGBUS:  return "SIGBUS (Bus Error)";
        default:      return "Unknown Signal";
    }
}

/**
 * @brief 信号处理函数
 */
void signalHandler(int sig) {
    std::cerr << std::endl << "接收到信号 " << sig << " (" << getSignalName(sig) << ")" << std::endl;
    utils::BackTrace::printStackTrace();
    signal(sig, SIG_DFL);
    raise(sig);
}

/**
 * @brief 打印统计信息
 */
void printStatistics() {
    uint64_t requests = total_requests.load();
    
    if (requests == 0 || !first_request_received.load()) {
        std::cout << "\n========== 服务器统计 ==========" << std::endl;
        std::cout << "还未收到请求..." << std::endl;
        std::cout << "总连接数: " << total_connections.load() << std::endl;
        std::cout << "活跃连接: " << active_connections.load() << std::endl;
        std::cout << "==============================\n" << std::endl;
        return;
    }
    
    // 计算从第一个请求到最后一个请求的时间（无锁读取）
    int64_t first_ns = first_request_time_ns.load();
    int64_t last_ns = last_request_time_ns.load();
    double elapsed_seconds = (last_ns - first_ns) / 1e9;
    
    // 至少1毫秒，避免除0
    if (elapsed_seconds < 0.001) elapsed_seconds = 0.001;
    
    uint64_t bytes_recv = total_bytes_received.load();
    uint64_t bytes_sent = total_bytes_sent.load();
    
    std::cout << "\n========== 服务器统计 ==========" << std::endl;
    std::cout << "总连接数: " << total_connections.load() << std::endl;
    std::cout << "活跃连接: " << active_connections.load() << std::endl;
    std::cout << "总请求数: " << requests << std::endl;
    std::cout << "接收字节: " << std::fixed << std::setprecision(2) 
              << (bytes_recv / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "发送字节: " << std::fixed << std::setprecision(2)
              << (bytes_sent / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "\n性能指标（基于实际请求时间）:" << std::endl;
    std::cout << "  请求处理时长: " << std::fixed << std::setprecision(3) 
              << elapsed_seconds << " 秒" << std::endl;
    std::cout << "  QPS: " << std::fixed << std::setprecision(0)
              << (requests / elapsed_seconds) << " 请求/秒" << std::endl;
    std::cout << "  吞吐量: " << std::fixed << std::setprecision(2)
              << (bytes_recv / elapsed_seconds / 1024.0 / 1024.0) << " MB/s" << std::endl;
    std::cout << "==============================\n" << std::endl;
}


int main(int argc, char* argv[]) 
{
    // 解析命令行参数
    std::string host = "0.0.0.0";
    uint32_t port = 8070;
    int backlog_size = 2048;
    
    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }
    if (argc >= 3) {
        backlog_size = std::stoi(argv[2]);
    }
    
    std::cout << "========== TCP压力测试服务器 ==========" << std::endl;
    std::cout << "监听地址: " << host << ":" << port << std::endl;
    std::cout << "Backlog: " << backlog_size << std::endl;
    std::cout << "======================================\n" << std::endl;
    
    // 初始化运行时
    RuntimeBuilder runtimeBuilder;
    Runtime runtime = runtimeBuilder.build();
    runtime.start();
    
    // 设置信号处理
    utils::SignalHandler::setSignalHandler<SIGSEGV>(signalHandler);
    utils::SignalHandler::setSignalHandler<SIGINT>([](int) {
        std::cout << "\n接收到中断信号，打印最终统计..." << std::endl;
        printStatistics();
        exit(0);
    });
    
    // 构建TCP服务器
    TcpServerBuilder builder;
    builder.addListen({host, port});
    TcpServer server = builder.backlog(backlog_size).build();
    
    // 运行服务器
    std::cout << "Starting TCP server..." << std::endl;
    bool run_result = server.run(runtime, [](AsyncTcpSocket socket, CoSchedulerHandle handle[[maybe_unused]]) -> Coroutine<nil> {
        using namespace error;
        
        total_connections++;
        active_connections++;
        
        Buffer buffer(4096);  // 使用较大的缓冲区
        
        while(true) {
            // 接收数据
            auto rwrapper = co_await socket.recv(buffer.data(), buffer.capacity());
            if (!rwrapper) {
                if(CommonError::contains(rwrapper.error().code(), ErrorCode::DisConnectError)) {
                    active_connections--;
                    co_await socket.close();
                    co_return nil();
                }
                active_connections--;
                co_return nil();
            }
            
            Bytes& bytes = rwrapper.value();
            size_t recv_size = bytes.size();
            
            // 记录请求时间（用于准确计算QPS）- 使用atomic无锁方式
            auto now = std::chrono::steady_clock::now();
            int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()
            ).count();
            
            if (!first_request_received.exchange(true)) {
                // 第一个请求
                first_request_time_ns.store(now_ns);
                last_request_time_ns.store(now_ns);
            } else {
                // 更新最后请求时间
                last_request_time_ns.store(now_ns);
            }
            
            // 更新统计
            total_requests++;
            total_bytes_received += recv_size;
            Bytes response = Bytes::fromString("HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!");
            // Echo回数据（纯粹的压测服务器，不处理特殊命令）
            auto wwrapper = co_await socket.send(std::move(response));
            if (wwrapper) {
                total_bytes_sent += recv_size;
            } else {
                std::cerr << "发送错误: " << wwrapper.error().message() << std::endl;
                active_connections--;
                co_return nil();
            }
        }
    });
    
    if (!run_result) {
        std::cerr << "Failed to start TCP server!" << std::endl;
        return 1;
    }
    std::cout << "TCP server started successfully!" << std::endl;
    std::cout << "Waiting for connections..." << std::endl;
    
    // 等待服务器结束
    server.wait();
    runtime.stop();
    
    // 打印最终统计
    std::cout << "\n服务器关闭，最终统计：" << std::endl;
    printStatistics();
    
    return 0;
}

