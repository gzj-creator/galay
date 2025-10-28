#include "SslEvent.h"
#include "Socket.h"
#include "galay/common/Log.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"

galay::AsyncSslSocket galay::AsyncSslSocketBuilder::build()
{
    if(m_ssl == nullptr) {
        LogError("SSL* is nullptr");
    }
    HandleOption option({SSL_get_fd(m_ssl)});
    option.handleNonBlock();
    return AsyncSslSocket(m_scheduler, m_ssl);
}

bool galay::AsyncSslSocketBuilder::check() const
{
    return m_ssl != nullptr;
}

namespace galay::details
{ 
    SslAcceptEvent::SslAcceptEvent(SSL* ssl, EventScheduler* scheduler)
        : SslEvent<std::expected<bool, CommonError>>(ssl, scheduler)
    {
    }

    void SslAcceptEvent::handleEvent()
    {
        m_waker.wakeUp();
    }

    EventType SslAcceptEvent::getEventType() const
    {
        switch (m_ssl_code)
        {
        case SSL_ERROR_WANT_READ:
            return EventType::kEventTypeRead;
        case SSL_ERROR_WANT_WRITE:
            return EventType::kEventTypeWrite;
        default:
            break;
        }
        return EventType::kEventTypeNone;
    }

    bool SslAcceptEvent::onReady()
    {
        m_ready = sslAccept(false);
        return m_ready;
    }

    std::expected<bool, CommonError> SslAcceptEvent::onResume()
    {
        if(!m_ready) sslAccept(true);
        return SslEvent<std::expected<bool, CommonError>>::onResume();
    }

    bool SslAcceptEvent::sslAccept(bool notify)
    {
        using namespace error;
        int r = SSL_do_handshake(m_ssl);
        LogTrace("[sslAccept] [handshake_return: {}] [notify: {}]", r, notify);
        if(r == 1) {
            LogInfo("[SSL handshake completed successfully]");
            m_result = true;
            return true;
        } 
        m_ssl_code = SSL_get_error(m_ssl, r);
        if( this->m_ssl_code == SSL_ERROR_WANT_READ || this->m_ssl_code == SSL_ERROR_WANT_WRITE ) {
            LogTrace("[SSL_do_handshake needs more data] [ssl_error: {}] [notify: {}]", m_ssl_code, notify);
            if( notify ) {
                m_result = false;
            }
            return false;
        } else {
            int fd = SSL_get_fd(m_ssl);
            LogError("[SSL_do_handshake failed] [return: {}] [ssl_error: {}] [errno: {}]", r, m_ssl_code, errno);
            SSL_free(m_ssl);
            close(fd);
            m_ssl = nullptr;
            m_result = std::unexpected(CommonError(CallSSLHandshakeError, m_ssl_code));
        }
        return true;
    }

    SslCloseEvent::SslCloseEvent(SSL* ssl, EventScheduler* scheduler)
        : SslEvent<std::expected<void, CommonError>>(ssl, scheduler)
    {
    }

    void SslCloseEvent::handleEvent()
    {
        if(sslClose()) {
            m_waker.wakeUp();
        } else {
            m_scheduler->activeEvent(this, nullptr);
        }
    }

    bool SslCloseEvent::sslClose()
    {
        using namespace error;
        int r = SSL_shutdown(m_ssl);
        if(r == 1) {
            close(SSL_get_fd(m_ssl));
            m_scheduler->removeEvent(this, nullptr);
            SSL_free(m_ssl);
            m_ssl = nullptr;
            
            return true;
        } else if(r == 0) {
            r = SSL_shutdown(m_ssl);
            if(r == 1) {
                close(SSL_get_fd(m_ssl));
                m_scheduler->removeEvent(this, nullptr);
                m_result = {};
                SSL_free(m_ssl);
                m_ssl = nullptr;
                return true;
            }
        }
        m_ssl_code = SSL_get_error(m_ssl, r);
        if( this->m_ssl_code == SSL_ERROR_WANT_READ || this->m_ssl_code == SSL_ERROR_WANT_WRITE ){
            return false;
        } else if( this->m_ssl_code == SSL_ERROR_ZERO_RETURN ) {
            // 对端关闭
            close(SSL_get_fd(m_ssl));
            m_scheduler->removeEvent(this, nullptr);
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
            SSL_free(m_ssl);
            m_ssl = nullptr;
            return true;
        } else {
            SSL_set_quiet_shutdown(m_ssl, 1);
            SSL_shutdown(m_ssl);
            close(SSL_get_fd(m_ssl));
            m_scheduler->removeEvent(this, nullptr);
            m_result = std::unexpected(CommonError(CallSSLShuntdownError, static_cast<uint32_t>(errno)));
            SSL_free(m_ssl);
            m_ssl = nullptr;
            return true;
        }
        return true;
    }

    EventType SslCloseEvent::getEventType() const
    {
        switch (m_ssl_code)
        {
        case SSL_ERROR_WANT_READ:
            return EventType::kEventTypeRead;
        case SSL_ERROR_WANT_WRITE:
            return EventType::kEventTypeWrite;
        default:
            break;
        }
        return EventType::kEventTypeNone;
    }

    bool SslCloseEvent::onReady()
    {
        return sslClose();
    }

    SslConnectEvent::SslConnectEvent(SSL* ssl, EventScheduler* scheduler)
        : SslEvent<std::expected<bool, CommonError>>(ssl, scheduler)
    {
    }

    void SslConnectEvent::handleEvent()
    {
        m_waker.wakeUp();
    }

    EventType SslConnectEvent::getEventType() const
    {
        switch (m_ssl_code)
        {
        case SSL_ERROR_WANT_READ:
            return EventType::kEventTypeRead;
        case SSL_ERROR_WANT_WRITE:
            return EventType::kEventTypeWrite;
        default:
            break;
        }
        return EventType::kEventTypeNone;
    }

    bool SslConnectEvent::onReady()
    {
        m_ready = sslConnect(false);
        return m_ready;
    }

    std::expected<bool, CommonError> SslConnectEvent::onResume()
    {
        if(!m_ready) sslConnect(true);
        return SslEvent<std::expected<bool, CommonError>>::onResume();
    }

    bool SslConnectEvent::sslConnect(bool notify)
    {
        using namespace error;
        int r = SSL_do_handshake(m_ssl);
        if(r == 1) {
            m_result = true;
            return true;
        } 
        m_ssl_code = SSL_get_error(m_ssl, r);
        if( this->m_ssl_code == SSL_ERROR_WANT_READ || this->m_ssl_code == SSL_ERROR_WANT_WRITE ){
            if( notify ) {
                m_result = false;
            }
            return false;
        } else {
            LogError("[SSL_do_handshake failed] [return: {}] [ssl_error: {}] [errno: {}]", r, m_ssl_code, errno);
            m_result = std::unexpected(CommonError(CallSSLHandshakeError, m_ssl_code));
        }
        return true;
    }

    SslRecvEvent::SslRecvEvent(SSL* ssl, EventScheduler* scheduler, char* result, size_t length)
        : SslEvent<std::expected<Bytes, CommonError>>(ssl, scheduler), m_length(length), m_result_str(result)
    {
    }

    void SslRecvEvent::handleEvent()
    {
        m_waker.wakeUp();
    }

    bool SslRecvEvent::onReady()
    {
        m_ready = sslRecv(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> SslRecvEvent::onResume()
    {
        if(!m_ready) sslRecv(true);
        return SslEvent<std::expected<Bytes, CommonError>>::onResume();
    }

    bool SslRecvEvent::sslRecv(bool notify)
    {
        using namespace error;
        Bytes bytes;
        int recvBytes = SSL_read(m_ssl, m_result_str, m_length);
        if(recvBytes > 0) LogTrace("recvBytes: {}, buffer: {}", recvBytes, std::string(m_result_str, recvBytes));
        if (recvBytes > 0) {
            bytes = Bytes::fromCString(m_result_str, recvBytes, recvBytes);
            m_result = std::move(bytes);
        } else if (recvBytes == 0) {
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallRecvError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

    SslSendEvent::SslSendEvent(SSL* ssl, EventScheduler* scheduler, Bytes &&bytes)
        : SslEvent<std::expected<Bytes, CommonError>>(ssl, scheduler), m_bytes(std::move(bytes))
    {
    }

    void SslSendEvent::handleEvent()
    {
        m_waker.wakeUp();
    }

    bool SslSendEvent::onReady()
    {
        m_ready = sslSend(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> SslSendEvent::onResume()
    {
        if(!m_ready) sslSend(true);
        return SslEvent<std::expected<Bytes, CommonError>>::onResume();
    }

    bool SslSendEvent::sslSend(bool notify)
    {
        using namespace error;
        int sendBytes = SSL_write(m_ssl, m_bytes.data(), m_bytes.size());
        if(sendBytes > 0) LogTrace("sendBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else if (sendBytes == 0) {
            m_result = Bytes();
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            } else if ( static_cast<uint32_t>(errno) == EPIPE || static_cast<uint32_t>(errno) == ECONNRESET ) {
                m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
                return true;
            }
            m_result = std::unexpected(CommonError(CallSendError, static_cast<uint32_t>(errno)));
        }
        return true;
    }
}