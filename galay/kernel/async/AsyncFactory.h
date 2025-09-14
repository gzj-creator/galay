#ifndef GALAY_ASYNC_FACTORY_H
#define GALAY_ASYNC_FACTORY_H 

#include "Socket.h"
#include "File.h"
#include "TimerGenerator.h"
#include "TaskRunner.h"

namespace galay
{
    class AsyncFactory
    { 
    public:
        AsyncFactory(Runtime& runtime, int index = -1);
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

        TaskRunner createTaskRunner();

    private:
        Runtime& m_runtime;
        int m_index;
    };
}

#endif