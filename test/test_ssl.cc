#include "galay/kernel/async/Socket.h"
#include "galay/common/Log.h"
#include "galay/kernel/async/Bytes.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Buffer.h"
#include <iostream>

using namespace galay;

Runtime runtime;
SSL_CTX* g_ssl_ctx = nullptr;

Coroutine<nil> Recv(AsyncSslSocket socket);
Coroutine<nil> Send(AsyncSslSocket socket);

Coroutine<nil> test()
{
    AsyncSslSocket socket(runtime, g_ssl_ctx);
    auto t1 = socket.socket();
    socket.options().handleNonBlock();
    socket.options().handleReusePort();
    if (!t1)
    {
        std::cout << t1.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "socket success" << std::endl;
    auto t2 = socket.bind({"127.0.0.1", 8070});
    if (!t2)
    {
        std::cout << t2.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "bind success" << std::endl;
    auto t3 = socket.listen(10);
    if (!t3)
    {
        std::cout << t3.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "listen success" << std::endl;
    while (true)
    {
        AsyncSslSocketBuilder builder(g_ssl_ctx);
        if(auto acceptor = co_await socket.accept(builder); !acceptor) {
            std::cout << acceptor.error().message() << std::endl;
            continue;
        } 
        if( auto res = socket.readyToSslAccept(builder); !res) {
            std::cout << res.error().message() << std::endl;
            continue;
        }
        std::expected<bool, CommonError> res;
        do
        {
            res = co_await socket.sslAccept(builder);
            if(!res) {
                std::cout << res.error().message() << std::endl;
            } 
            if(res.value()) {
                std::cout << "ssl accept success" << std::endl;
                break;
            }
        } while (true);
        if(!res) continue;
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
        if(!wrapper) {
            if( CommonError::contains(wrapper.error().code(), ErrorCode::DisConnectError)) {
                // disconnect
                co_await socket.sslClose();
                std::cout << "connection close" << std::endl;
                co_return nil();
            }
            std::cout << wrapper.error().message() << std::endl;
            co_return nil();
        }
        Bytes& bytes = wrapper.value();
        std::string msg = bytes.toString();
        std::cout << msg.length() << "   " <<  msg << std::endl;
        if (msg.find("quit") != std::string::npos)
        {
            auto success = co_await socket.sslClose();
            if (success)
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
    if (wrapper)
    {
        Bytes& remain = wrapper.value();
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
    
    // Initialize SSL context
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    if(!g_ssl_ctx) {
        std::cerr << "Failed to create SSL context" << std::endl;
        return 1;
    }
    if(SSL_CTX_use_certificate_file(g_ssl_ctx, "server.crt", SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "Failed to load certificate" << std::endl;
        return 1;
    }
    if(SSL_CTX_use_PrivateKey_file(g_ssl_ctx, "server.key", SSL_FILETYPE_PEM) <= 0) {
        std::cerr << "Failed to load private key" << std::endl;
        return 1;
    }
    
    runtime = builder.build();
    runtime.start();
    runtime.schedule(test());
    getchar();
    runtime.stop();
    
    // Cleanup SSL context
    if(g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        EVP_cleanup();
    }
    return 0;
}