#include "AsyncFactory.h"
#include <cassert>

namespace galay
{
    AsyncFactory::AsyncFactory(Runtime* runtime)
        : m_runtime(runtime)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
    }

    AsyncFactory::AsyncFactory(const AsyncFactory& other)
        : m_runtime(other.m_runtime)
    {
    }

    AsyncFactory::AsyncFactory(AsyncFactory&& other) noexcept
        : m_runtime(other.m_runtime)
    {
        other.m_runtime = nullptr;
    }

    AsyncFactory& AsyncFactory::operator=(const AsyncFactory& other)
    {
        if (this != &other) {
            m_runtime = other.m_runtime;
        }
        return *this;
    }

    AsyncFactory& AsyncFactory::operator=(AsyncFactory&& other) noexcept
    {
        if (this != &other) {
            m_runtime = other.m_runtime;
            other.m_runtime = nullptr;
        }
        return *this;
    }


    AsyncTcpSocket AsyncFactory::getTcpSocket()
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return AsyncTcpSocket(m_runtime);
    }

    AsyncTcpSocket AsyncFactory::getTcpSocket(GHandle handle)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return AsyncTcpSocket(m_runtime, handle);
    }

    AsyncUdpSocket AsyncFactory::getUdpSocket()
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return AsyncUdpSocket(m_runtime);
    }

    AsyncUdpSocket AsyncFactory::getUdpSocket(GHandle handle)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return AsyncUdpSocket(m_runtime, handle);
    }

    AsyncSslSocket AsyncFactory::getSslSocket(SSL_CTX* ssl_ctx)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return AsyncSslSocket(m_runtime, ssl_ctx);
    }

    AsyncSslSocket AsyncFactory::getSslSocket(SSL *ssl)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return AsyncSslSocket(m_runtime, ssl);
    }

    File AsyncFactory::getFile()
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return File(m_runtime);
    }

    File AsyncFactory::getFile(GHandle handle)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return File(m_runtime, handle);
    }

    TimerGenerator AsyncFactory::getTimerGenerator()
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        return TimerGenerator(m_runtime);
    }

}