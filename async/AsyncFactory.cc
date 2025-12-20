#include "AsyncFactory.h"
#include <cassert>

namespace galay
{
    AsyncFactory::AsyncFactory(CoSchedulerHandle handle)
        : m_handle(handle)
    {
    }

    AsyncFactory::AsyncFactory(const AsyncFactory& other)
        : m_handle(other.m_handle)
    {
    }

    AsyncFactory::AsyncFactory(AsyncFactory&& other) noexcept
        : m_handle(other.m_handle)
    {
    }

    AsyncFactory& AsyncFactory::operator=(const AsyncFactory& other)
    {
        if (this != &other) {
            m_handle = other.m_handle;
        }
        return *this;
    }

    AsyncFactory& AsyncFactory::operator=(AsyncFactory&& other) noexcept
    {
        if (this != &other) {
            m_handle = other.m_handle;
        }
        return *this;
    }


    AsyncTcpSocket AsyncFactory::getTcpSocket()
    {
        return AsyncTcpSocket(m_handle);
    }

    AsyncTcpSocket AsyncFactory::getTcpSocket(GHandle handle)
    {
        return AsyncTcpSocket(m_handle, handle);
    }

    AsyncUdpSocket AsyncFactory::getUdpSocket()
    {
        return AsyncUdpSocket(m_handle);
    }

    AsyncUdpSocket AsyncFactory::getUdpSocket(GHandle handle)
    {
        return AsyncUdpSocket(m_handle, handle);
    }

    AsyncSslSocket AsyncFactory::getSslSocket(SSL_CTX *ssl_ctx)
    {
        return AsyncSslSocket(m_handle, ssl_ctx);
    }

    AsyncSslSocket AsyncFactory::getSslSocket(SSL *ssl)
    {
        return AsyncSslSocket(m_handle, ssl);
    }

    File AsyncFactory::getFile()
    {
        return File(m_handle);
    }

    File AsyncFactory::getFile(GHandle handle)
    {
        return File(m_handle, handle);
    }

    TimerGenerator AsyncFactory::getTimerGenerator()
    {
        return TimerGenerator(m_handle);
    }
}
