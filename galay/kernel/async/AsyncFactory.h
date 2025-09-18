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
    public:
        AsyncFactory(Runtime& runtime);
        AsyncTcpSocket createTcpSocket();
        AsyncTcpSocket createTcpSocket(GHandle handle);
        
        AsyncUdpSocket createUdpSocket();
        AsyncUdpSocket createUdpSocket(GHandle handle);
        AsyncSslSocket createSslSocket();
        //SSL_set_fd must be called before
        AsyncSslSocket createSslSocket(SSL* ssl);

        File createFile();
        File createFile(GHandle handle);
        TimerGenerator createTimerGenerator();

        TaskRunner createTaskRunner(int co_id);
    private:
        Runtime& m_runtime;
    };
}

#endif