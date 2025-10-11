#include "AsyncFactory.h"

namespace galay
{
    AsyncFactory::AsyncFactory(Runtime &runtime)
        : m_runtime(runtime)
    {
    }

    AsyncFactory::AsyncFactory(const AsyncFactory &other)
        : m_runtime(other.m_runtime)
    {
    }


    AsyncTcpSocket AsyncFactory::getTcpSocket()
    {
        return AsyncTcpSocket(m_runtime);
    }

    AsyncTcpSocket AsyncFactory::getTcpSocket(GHandle handle)
    {
        return AsyncTcpSocket(m_runtime, handle);
    }

    AsyncUdpSocket AsyncFactory::getUdpSocket()
    {
        return AsyncUdpSocket(m_runtime);
    }

    AsyncUdpSocket AsyncFactory::getUdpSocket(GHandle handle)
    {
        return AsyncUdpSocket(m_runtime, handle);
    }

    AsyncSslSocket AsyncFactory::getSslSocket()
    {
        return AsyncSslSocket(m_runtime);
    }

    AsyncSslSocket AsyncFactory::getSslSocket(SSL *ssl)
    {
        return AsyncSslSocket(m_runtime, ssl);
    }

    File AsyncFactory::getFile()
    {
        return File(m_runtime);
    }

    File AsyncFactory::getFile(GHandle handle)
    {
        return File(m_runtime, handle);
    }

    TimerGenerator AsyncFactory::getTimerGenerator()
    {
        return TimerGenerator(m_runtime);
    }

    TaskRunner AsyncFactory::getTaskRunner(int co_id)
    {
        return TaskRunner(m_runtime, co_id);
    }
}