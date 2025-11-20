#include "galay/common/Error.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include <iostream>
#include <atomic>

using namespace galay;

CoSchedulerHandle handle;
std::atomic<uint64_t> total_recv_bytes{0};
std::atomic<uint64_t> total_send_bytes{0};
std::atomic<uint32_t> active_connections{0};

// 高性能双向通信处理器
Coroutine<nil> handleConnection(AsyncTcpSocket socket, uint32_t conn_id)
{
    active_connections.fetch_add(1);
    std::cout << "[连接 " << conn_id << "] 建立，fd: " << socket.getHandle().fd << std::endl;
    
    Buffer recv_buffer(4096);  // 使用更大的缓冲区提高性能
    bool running = true;
    
    while (running) {
        // 接收数据
        auto recv_result = co_await socket.recv(recv_buffer.data(), recv_buffer.capacity());
        if (!recv_result) {
            std::cout << "[连接 " << conn_id << "] 接收错误: " 
                      << recv_result.error().message() << std::endl;
            break;
        }
        
        Bytes received = std::move(recv_result.value());
        if (received.size() == 0) {
            std::cout << "[连接 " << conn_id << "] 对端关闭连接" << std::endl;
            break;
        }
        
        size_t recv_size = received.size();
        total_recv_bytes.fetch_add(recv_size);
        std::string msg = received.toString();
        
        // 处理特殊命令
        if (msg == "quit" || msg == "exit") {
            std::cout << "[连接 " << conn_id << "] 收到退出命令" << std::endl;
            running = false;
        }
        
        // 回显数据（高性能场景）
        auto send_result = co_await socket.send(std::move(received));
        if (!send_result) {
            std::cout << "[连接 " << conn_id << "] 发送错误: " 
                      << send_result.error().message() << std::endl;
            break;
        }
        
        total_send_bytes.fetch_add(send_result.value().size());
        
        // 让出 CPU，允许其他协程执行
        co_yield nil();
    }
    
    // 关闭连接
    co_await socket.close();
    active_connections.fetch_sub(1);
    std::cout << "[连接 " << conn_id << "] 关闭" << std::endl;
    
    co_return nil();
}

Coroutine<nil> acceptLoop()
{
    std::cout << "启动服务器..." << std::endl;
    
    auto socket = handle.getAsyncFactory().getTcpSocket();
    
    if (auto res = socket.socket(); !res) {
        std::cout << "创建 socket 失败: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    auto options = socket.options();
    if (auto res = options.handleReuseAddr(); !res) {
        std::cout << "设置 REUSEADDR 失败: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    if (auto res = socket.bind({"0.0.0.0", 8070}); !res) {
        std::cout << "绑定失败: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    if (auto res = socket.listen(2048); !res) {
        std::cout << "监听失败: " << res.error().message() << std::endl;
        co_return nil();
    }
    
    std::cout << "服务器监听在 0.0.0.0:8070" << std::endl;
    std::cout << "活跃连接数将实时更新..." << std::endl;
    
    uint32_t conn_counter = 0;
    
    while (true) {
        AsyncTcpSocketBuilder builder;
        auto result = co_await socket.accept(builder);
        
        if (result) {
            auto new_socket = builder.build();
            conn_counter++;
            
            // 为每个连接启动一个协程
            handle.spawn(handleConnection(std::move(new_socket), conn_counter));
            
            // 每 100 个连接打印一次统计
            if (conn_counter % 100 == 0) {
                std::cout << "总连接数: " << conn_counter 
                          << ", 活跃: " << active_connections.load()
                          << ", 接收: " << (total_recv_bytes.load() / 1024.0 / 1024.0) << " MB"
                          << ", 发送: " << (total_send_bytes.load() / 1024.0 / 1024.0) << " MB"
                          << std::endl;
            }
        } else {
            std::cout << "accept 失败: " << result.error().message() << std::endl;
        }
        
        co_yield nil();
    }
}

// 统计协程
Coroutine<nil> statsLoop()
{
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_recv = 0;
    uint64_t last_send = 0;
    
    while (true) {
        // 每 5 秒打印一次统计
        for (int i = 0; i < 50; ++i) {
            co_yield nil();
        }
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        if (elapsed > 0 && elapsed % 5 == 0) {
            uint64_t curr_recv = total_recv_bytes.load();
            uint64_t curr_send = total_send_bytes.load();
            
            double recv_rate = (curr_recv - last_recv) / 5.0 / 1024.0 / 1024.0;
            double send_rate = (curr_send - last_send) / 5.0 / 1024.0 / 1024.0;
            
            std::cout << "\n========== 性能统计 =========="
                      << "\n活跃连接: " << active_connections.load()
                      << "\n接收速率: " << recv_rate << " MB/s"
                      << "\n发送速率: " << send_rate << " MB/s"
                      << "\n总接收: " << (curr_recv / 1024.0 / 1024.0) << " MB"
                      << "\n总发送: " << (curr_send / 1024.0 / 1024.0) << " MB"
                      << "\n============================\n" << std::endl;
            
            last_recv = curr_recv;
            last_send = curr_send;
        }
    }
}

int main() {
    // 设置日志级别
    log::enable(spdlog::level::debug);
    
    std::cout << "========== 高性能双向通信服务器 ==========" << std::endl;
    std::cout << "特性:" << std::endl;
    std::cout << "  - 协程驱动的异步 I/O" << std::endl;
    std::cout << "  - 同时处理读写事件" << std::endl;
    std::cout << "  - 零拷贝数据传输" << std::endl;
    std::cout << "  - 实时性能统计" << std::endl;
    std::cout << "==========================================" << std::endl;
    
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(100));
    auto runtime = builder.build();
    runtime.start();
    
    handle = runtime.schedule(acceptLoop());
    // 启动统计协程
    runtime.schedule(statsLoop());
    
    std::cout << "\n按回车键停止服务器..." << std::endl;
    getchar();
    
    std::cout << "\n正在关闭服务器..." << std::endl;
    runtime.stop();
    
    std::cout << "\n========== 最终统计 ==========" << std::endl;
    std::cout << "总接收: " << (total_recv_bytes.load() / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "总发送: " << (total_send_bytes.load() / 1024.0 / 1024.0) << " MB" << std::endl;
    std::cout << "=============================" << std::endl;
    
    return 0;
}

