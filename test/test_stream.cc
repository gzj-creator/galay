#include "galay/common/Error.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include <chrono>
#include <iostream>
#include <ratio>

using namespace galay;

CoSchedulerHandle handle;

std::condition_variable cond;

std::atomic_bool start = false;

Coroutine<nil> testRead(AsyncTcpSocket socket)
{
    std::cout << "testRead, socket fd: " << socket.getHandle().fd << std::endl;
    std::cout << "testRead: waiting for data..." << std::endl;
    Buffer buffer(1024);
    while (true)
    {
        std::cout << "testRead: calling recv..." << std::endl;
        auto result = co_await socket.recv(buffer.data(), buffer.capacity());
        std::cout << "testRead: recv returned" << std::endl;
        if(!result) {
            std::cout << "testRead: recv error: " << result.error().message() << std::endl;
            co_return nil();
        }
        std::string msg = result.value().toString();
        std::cout << "testRead: received: " << msg << std::endl;
        if(msg == "start") {
            start = true;
            std::cout << "testRead: start signal received!" << std::endl;
        } else if(msg == "quit") {
            start = false;
            co_await socket.close();
            std::cout << "testRead close" << std::endl;
            co_return nil();
        }
        co_yield nil();
    }
    co_return nil();
}

Coroutine<nil> testWrite(AsyncTcpSocket socket)
{
    std::cout << "testWrite, socket fd: " << socket.getHandle().fd << std::endl;
    std::cout << "testWrite: waiting for start signal..." << std::endl;
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    // 使用 co_yield 而不是阻塞等待，让出 CPU 给其他协程
    // 但不要太频繁 yield，给其他协程机会
    while(!start) {
        co_await generator.sleep(std::chrono::milliseconds(3000));
        std::cout << "sleep over" << std::endl;    
    }
    std::cout << "ready to send" << std::endl;
    while(start) {
        auto res = co_await socket.send(Bytes::fromString("----------------hello world-----------------"));
        if(!res) {
            std::cout << "send error: " << res.error().message() << std::endl;
            co_return nil();
        }
        std::cout << "sent message" << std::endl;
        co_await generator.sleep(std::chrono::milliseconds(1000));
    }
    co_return nil();
}

Coroutine<nil> test()
{
    std::cout << "test start" << std::endl;
    auto socket = handle.getAsyncFactory().getTcpSocket();
    if(auto res = socket.socket(); !res) {
        std::cout << res.error().message() << std::endl;
        co_return nil();
    }
    auto options = socket.options();
    if(auto res = options.handleReuseAddr(); ! res) {
        std::cout << res.error().message() << std::endl;
        co_return nil();
    }
    if(auto res = socket.bind({"127.0.0.1", 8070}); !res) {
        std::cout << res.error().message() << std::endl;
        co_return nil();
    }
    if(auto res = socket.listen(1024); !res ) {
        std::cout << res.error().message() << std::endl;
        co_return nil();
    }
    while (true)
    {
        AsyncTcpSocketBuilder builder;
        auto result = co_await socket.accept(builder);
        if(result) {
            auto new_socket = builder.build();
            std::cout << "accept success" << std::endl;
            handle.spawn(testRead(new_socket.clone()));
            handle.spawn(testWrite(new_socket.clone()));
        } else {
            std::cout << "accept failed: " << result.error().message() << std::endl;
        }
        
    }
    
}

int main() { 
    details::InternelLogger().getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::debug);
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(10));  // 减少到 10ms，提高调度频率
    auto runtime = builder.build();
    runtime.start();
    handle = runtime.schedule(test());
    std::cout << "waiting......" << std::endl;
    getchar();
    runtime.stop();
    return 0;
}