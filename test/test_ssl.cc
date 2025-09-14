#include "galay/kernel/async/Socket.h"
#include "galay/common/Log.h"
#include "galay/kernel/async/Bytes.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Buffer.h"
#include <iostream>

using namespace galay;

Runtime runtime;

Coroutine<nil> Recv(AsyncSslSocket socket);
Coroutine<nil> Send(AsyncSslSocket socket);

Coroutine<nil> test()
{
    AsyncSslSocket socket(runtime);
    auto t1 = socket.socket();
    socket.options().handleNonBlock();
    socket.options().handleReusePort();
    if (!t1.success())
    {
        std::cout << t1.getError()->message() << std::endl;
        co_return nil();
    }
    std::cout << "socket success" << std::endl;
    auto t2 = socket.bind({"127.0.0.1", 8070});
    if (!t2.success())
    {
        std::cout << t2.getError()->message() << std::endl;
        co_return nil();
    }
    std::cout << "bind success" << std::endl;
    auto t3 = socket.listen(10);
    if (!t3.success())
    {
        std::cout << t3.getError()->message() << std::endl;
        co_return nil();
    }
    std::cout << "listen success" << std::endl;
    while (true)
    {
        auto t4 = co_await socket.sslAccept();
        if (!t4.success())
        {
            std::cout << t4.getError()->message() << std::endl;
            co_return nil();
        }
        std::cout << "accept success" << std::endl;
        auto builder = t4.moveValue();
        auto new_socket = builder.build();
        new_socket.options().handleNonBlock();
        runtime.schedule(Recv(std::move(new_socket)));
    }
}

Coroutine<nil> Recv(AsyncSslSocket socket)
{
    Buffer buffer(1024);
    while (true)
    {
        auto wrapper = co_await socket.sslRecv(buffer.data(), buffer.capacity());
        if(!wrapper.success()) {
            if(wrapper.getError()->code() == error::ErrorCode::DisConnectError) {
                // disconnect
                co_await socket.sslClose();
                std::cout << "connection close" << std::endl;
                co_return nil();
            }
            std::cout << wrapper.getError()->message() << std::endl;
            co_return nil();
        }
        Bytes bytes = wrapper.moveValue();
        std::string msg = bytes.toString();
        std::cout << msg.length() << "   " <<  msg << std::endl;
        if (msg.find("quit") != std::string::npos)
        {
            auto success = co_await socket.sslClose();
            if (success.success())
            {
                std::cout << "close success" << std::endl;
            }
            co_return nil();
        }
        runtime.schedule(Send(std::move(socket)));
    }
}

Coroutine<nil> Send(AsyncSslSocket socket)
{
    const char* data = "Hello World";
    Bytes bytes(data, 11);
    auto wrapper = co_await socket.sslSend(std::move(bytes));
    if (wrapper.success())
    {
        Bytes remain = wrapper.moveValue();
        std::cout << remain.toString()  << std::endl;
    }
    co_return nil();
}


int main()
{
    galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::trace);
    RuntimeBuilder builder;
    builder.setCoSchedulerNum(4)
            .startCoManager(std::chrono::milliseconds(1000))
            .setEventCheckTimeout(5)
            .setEventSchedulerInitFdsSize(1024);
    initializeSSLServerEnv("server.crt", "server.key");
    runtime = builder.build();
    runtime.start();
    runtime.schedule(test());
    getchar();
    runtime.stop();
    return 0;
}