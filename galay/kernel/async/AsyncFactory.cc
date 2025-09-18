#include "AsyncFactory.h"

namespace galay
{
    AsyncFactory::AsyncFactory(Runtime &runtime)
        : m_runtime(runtime)
    {
    }

    AsyncTcpSocket AsyncFactory::createTcpSocket()
    {
        return AsyncTcpSocket(m_runtime);
    }

    AsyncTcpSocket AsyncFactory::createTcpSocket(GHandle handle)
    {
        return AsyncTcpSocket(m_runtime, handle);
    }

    AsyncUdpSocket AsyncFactory::createUdpSocket()
    {
        return AsyncUdpSocket(m_runtime);
    }

    AsyncUdpSocket AsyncFactory::createUdpSocket(GHandle handle)
    {
        return AsyncUdpSocket(m_runtime, handle);
    }

    AsyncSslSocket AsyncFactory::createSslSocket()
    {
        return AsyncSslSocket(m_runtime);
    }

    AsyncSslSocket AsyncFactory::createSslSocket(SSL *ssl)
    {
        return AsyncSslSocket(m_runtime, ssl);
    }

    File AsyncFactory::createFile()
    {
        return File(m_runtime);
    }

    File AsyncFactory::createFile(GHandle handle)
    {
        return File(m_runtime, handle);
    }

    TimerGenerator AsyncFactory::createTimerGenerator()
    {
        return TimerGenerator(m_runtime);
    }

    TaskRunner AsyncFactory::createTaskRunner(int co_id)
    {
        return TaskRunner(m_runtime, co_id);
    }
}