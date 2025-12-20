#ifndef GALAY_ASYNC_FACTORY_H
#define GALAY_ASYNC_FACTORY_H 

#include "Socket.h"
#include "File.h"
#include "TimerGenerator.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"

namespace galay
{
    //同一运行时同一时间只允许出现一次同一异步对象的异步操作
    //但可跨运行时实现同一异步对象的异步操作
    class AsyncFactory
    { 
        friend class Runtime;
        friend class CoSchedulerHandle;
    public:
        AsyncFactory() = default;
        AsyncFactory(const AsyncFactory& other);
        AsyncFactory(AsyncFactory&& other) noexcept;

        AsyncFactory& operator=(const AsyncFactory& other);
        AsyncFactory& operator=(AsyncFactory&& other) noexcept;
        
        AsyncTcpSocket getTcpSocket();
        AsyncTcpSocket getTcpSocket(GHandle handle);
        
        AsyncUdpSocket getUdpSocket();
        AsyncUdpSocket getUdpSocket(GHandle handle);
        AsyncSslSocket getSslSocket(SSL_CTX* ssl_ctx);
        //SSL_set_fd must be called before
        AsyncSslSocket getSslSocket(SSL* ssl);

        File getFile();
        File getFile(GHandle handle);
        TimerGenerator getTimerGenerator();

    private:
        AsyncFactory(CoSchedulerHandle handle);
    private:
        CoSchedulerHandle m_handle;
    };
}

#endif