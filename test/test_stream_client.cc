#include "galay/common/Log.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/client/TcpClient.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include <iostream>
#include <thread>
#include <queue>

using namespace galay;

CoSchedulerHandle handle;

std::atomic_bool start = false;

std::condition_variable cond;
std::mutex mtx;

// 输入队列，用于从输入线程传递消息到协程
std::queue<std::string> input_queue;
std::mutex input_mutex;
std::condition_variable input_cv;
std::atomic_bool input_thread_running{true};

Coroutine<nil> testRead(TcpClient client)
{
    std::cout << "testRead started" << std::endl;
    Buffer buffer(1024);
    int recv_count = 0;
    while (true)
    {
        std::cout << "testRead: [" << recv_count++ << "] waiting for data..." << std::endl;
        auto result = co_await client.recv(buffer.data(), buffer.capacity());
        std::cout << "testRead: recv returned, success=" << result.has_value() << std::endl;
        if(!result) {
            std::cout << "testRead: recv error: " << result.error().message() << std::endl;
            co_return nil();
        }
        std::string msg = result.value().toString();
        std::cout << "testRead: received [" << msg.length() << " bytes]: " << msg << std::endl;
        co_yield nil();
    }
    
}

Coroutine<nil> testWrite(TcpClient client)
{
    std::cout << "testWrite started" << std::endl;
    
    // 简化版本：只发送两条消息用于测试
    std::cout << "testWrite: sending 'start'" << std::endl;
    auto res1 = co_await client.send(Bytes::fromString("start"));
    if (!res1) {
        std::cout << "send error: " << res1.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "send success: start" << std::endl;
    
    // 等待一段时间，让服务器处理
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    co_await generator.sleep(std::chrono::milliseconds(5000));
    
    std::cout << "testWrite: sending 'quit'" << std::endl;
    auto res2 = co_await client.send(Bytes::fromString("quit"));
    if (!res2) {
        std::cout << "send error: " << res2.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "send success: quit" << std::endl;
    
    // 等待一段时间，让 testRead 有机会读取数据
    co_await generator.sleep(std::chrono::milliseconds(2000));
    
    co_await client.close();
    input_thread_running.store(false);
    cond.notify_all();
    co_return nil();
}

Coroutine<nil> test()
{
    std::cout << "test start" << std::endl;
    TcpClient client(handle);
    auto res1 = co_await client.connect({"127.0.0.1", 8070});
    if (!res1) {
        std::cout << "connect error: " << res1.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "connect success, fd: " << client.getHandle().fd << std::endl;
    auto write_client = client.clone();
    auto read_client = client.clone();
    std::cout << "write_client fd: " << write_client.getHandle().fd << std::endl;
    std::cout << "read_client fd: " << read_client.getHandle().fd << std::endl;
    handle.spawn(testWrite(std::move(write_client)));
    handle.spawn(testRead(std::move(read_client)));
    co_return nil();
}

int main() { 
    log::enable(spdlog::level::debug);
    
    // 启动输入线程，专门处理 std::getline
    std::thread input_thread([]() {
        std::cout << "Input thread started. Type messages (type 'quit' to exit):" << std::endl;
        while (input_thread_running.load()) {
            std::string line;
            std::getline(std::cin, line);
            if (!line.empty()) {
                std::unique_lock<std::mutex> lock(input_mutex);
                input_queue.push(line);
                input_cv.notify_one();
                
                if (line == "quit") {
                    input_thread_running.store(false);
                    break;
                }
            }
        }
        std::cout << "Input thread stopped" << std::endl;
    });
    
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(10));  // 减少到 10ms，提高调度频率
    auto runtime = builder.build();
    runtime.start();
    handle = runtime.schedule(test());
    std::cout << "waiting......" << std::endl;
    std::unique_lock lock(mtx);
    cond.wait(lock);
    runtime.stop();
    
    // 等待输入线程结束
    input_thread_running.store(false);
    input_cv.notify_all();
    if (input_thread.joinable()) {
        input_thread.join();
    }
    
    return 0;
}