#include "AsyncFactory.h"

namespace galay
{
    AsyncFactory::AsyncFactory(Runtime &runtime, int index)
        : m_runtime(runtime), m_index(index)
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

    TaskRunner AsyncFactory::createTaskRunner()
    {
        return TaskRunner(m_runtime, m_index);
    }
}