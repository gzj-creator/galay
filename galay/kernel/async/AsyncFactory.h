#ifndef GALAY_ASYNC_FACTORY_H
#define GALAY_ASYNC_FACTORY_H 

#include "Socket.h"
#include "File.h"
#include "TimerGenerator.h"
#include "TaskRunner.h"

namespace galay
{
    //同一运行时同一时间只允许出现一次同一异步对象的异步操作
    //但可跨运行时实现同一异步对象的异步操作
    class AsyncFactory
    { 
        friend class Runtime;
    public:

        AsyncFactory(const AsyncFactory& other);

        AsyncFactory& operator=(const AsyncFactory& other) = delete;
        
        AsyncTcpSocket getTcpSocket();
        AsyncTcpSocket getTcpSocket(GHandle handle);
        
        AsyncUdpSocket getUdpSocket();
        AsyncUdpSocket getUdpSocket(GHandle handle);
        AsyncSslSocket getSslSocket();
        //SSL_set_fd must be called before
        AsyncSslSocket getSslSocket(SSL* ssl);

        File getFile();
        File getFile(GHandle handle);
        TimerGenerator getTimerGenerator();

        TaskRunner getTaskRunner(int co_id);
    private:
        AsyncFactory(Runtime& runtime);
    private:
        Runtime& m_runtime;
    };
}

#endif