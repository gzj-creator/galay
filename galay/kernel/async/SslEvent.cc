#include "SslEvent.h"
#include "Socket.h"
#include "galay/common/Log.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"

galay::AsyncSslSocketBuilder galay::AsyncSslSocketBuilder::create(EventScheduler* scheduler, SSL *ssl){
    AsyncSslSocketBuilder builder;
    builder.m_ssl = ssl;
    builder.m_scheduler = scheduler;
    return builder;
}

galay::AsyncSslSocket galay::AsyncSslSocketBuilder::build()
{
    if(m_ssl == nullptr) {
        LogError("SSL* is nullptr");
    }
    return AsyncSslSocket(m_scheduler, m_ssl);
}

bool galay::AsyncSslSocketBuilder::check() const
{
    return m_ssl != nullptr;
}

namespace galay::details
{ 
    SslAcceptEvent::SslAcceptEvent(SSL* ssl, EventScheduler* scheduler)
        : SslEvent<std::expected<AsyncSslSocketBuilder, CommonError>>(ssl, scheduler), m_status(SslAcceptStatus::kSslAcceptStatus_Accept)
    {
    }

    void SslAcceptEvent::handleEvent()
    {
        if(sslAccept()) {
            m_waker.wakeUp();
        } else {
            m_scheduler->activeEvent(this, nullptr);
        }
    }

    EventType SslAcceptEvent::getEventType() const
    {
        if(m_status == SslAcceptStatus::kSslAcceptStatus_Accept) {
            return EventType::kEventTypeRead;
        } else if(m_status == SslAcceptStatus::kSslAcceptStatus_SslAccept) {
            switch (m_ssl_code)
            {
            case SSL_ERROR_WANT_READ:
                return EventType::kEventTypeRead;
            case SSL_ERROR_WANT_WRITE:
                return EventType::kEventTypeWrite;
            default:
                break;
            }
        }
        return EventType::kEventTypeNone;
    }

    bool SslAcceptEvent::onReady()
    {
        return sslAccept();
    }

    bool SslAcceptEvent::sslAccept()
    {
        using namespace error;
        if(m_status == SslAcceptStatus::kSslAcceptStatus_Accept) {
            //accept
            sockaddr addr{};
            socklen_t addr_len = sizeof(addr);
            GHandle handle {
                .fd = accept(SSL_get_fd(m_ssl), &addr, &addr_len),
            };
            if( handle.fd < 0 ) {
                if( static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR ) {
                    return false;
                }
                m_result = std::unexpected(CommonError(CallAcceptError, static_cast<uint32_t>(errno)));
                return true;
            }
            std::string ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
            uint16_t port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
            LogTrace("[Accept Address: {}:{}]", ip, port);
            m_accept_ssl = SSL_new(getGlobalSSLCtx());
            if(m_accept_ssl == nullptr) {
                close(handle.fd);
                m_result = std::unexpected(CommonError(CallSSLNewError, static_cast<uint32_t>(errno)));
                return true;
            }
            if(SSL_set_fd(m_accept_ssl, handle.fd) == -1) {
                SSL_free(m_accept_ssl);
                m_accept_ssl = nullptr;
                close(handle.fd);
                m_result = std::unexpected(CommonError(CallSSLSetFdError, static_cast<uint32_t>(errno)));
                return true;
            }
            SSL_set_accept_state(m_accept_ssl);
            m_status = SslAcceptStatus::kSslAcceptStatus_SslAccept;
        }
        if(m_status == SslAcceptStatus::kSslAcceptStatus_SslAccept) {
            int r = SSL_do_handshake(m_accept_ssl);
            if(r == 1) {
                m_result = AsyncSslSocketBuilder::create(m_scheduler, m_accept_ssl);
                return true;
            } 
            m_ssl_code = SSL_get_error(m_accept_ssl, r);
            if( this->m_ssl_code == SSL_ERROR_WANT_READ || this->m_ssl_code == SSL_ERROR_WANT_WRITE ){
                return false;
            } else {
                SSL_free(m_accept_ssl);
                close(SSL_get_fd(m_accept_ssl));
                m_accept_ssl = nullptr;
                m_result = std::unexpected(CommonError(CallSSLHandshakeError, static_cast<uint32_t>(errno)));
            }
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

    SslConnectEvent::SslConnectEvent(SSL* ssl, EventScheduler* scheduler, const Host &host)
        : SslEvent<std::expected<void, CommonError>>(ssl, scheduler), m_host(host)
    {
    }

    void SslConnectEvent::handleEvent()
    {
        if(sslConnect()) {
            m_waker.wakeUp();
        } else {
            m_scheduler->activeEvent(this, nullptr);
        }
    }

    EventType SslConnectEvent::getEventType() const
    {
        if(m_status == ConnectState::kConnectState_Connect) {
            return EventType::kEventTypeWrite;
        } else {
            switch (m_ssl_code)
            {
            case SSL_ERROR_WANT_READ:
                return EventType::kEventTypeRead;
            case SSL_ERROR_WANT_WRITE:
                return EventType::kEventTypeWrite;
            default:
                break;
            }
        }
        return EventType::kEventTypeNone;
    }

    bool SslConnectEvent::onReady()
    {
        return sslConnect();
    }

    bool SslConnectEvent::sslConnect()
    {
        using namespace error;
        if(m_status == ConnectState::kConnectState_Ready) {
            m_status = ConnectState::kConnectState_Connect;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr(m_host.ip.c_str());
            addr.sin_port = htons(m_host.port);
            const int ret = connect(SSL_get_fd(m_ssl), reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_in));
            if( ret != 0) {
                if( static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR || static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EINPROGRESS) {
                    m_status = ConnectState::kConnectState_Connect;
                    return false;
                }
                m_result = std::unexpected(CommonError(CallConnectError, static_cast<uint32_t>(errno)));
                return true;
            }
            m_status = ConnectState::kConnectState_Connect;
        }
        if(m_status == ConnectState::kConnectState_Connect) {
            m_status = ConnectState::kConnectState_SslConnect;
            SSL_set_connect_state(m_ssl);
        }
        if(m_status == ConnectState::kConnectState_SslConnect) {
            int r = SSL_do_handshake(m_ssl);
            if(r == 1) {
                m_result = {};
                return true;
            } 
            m_ssl_code = SSL_get_error(m_ssl, r);
            if( this->m_ssl_code == SSL_ERROR_WANT_READ || this->m_ssl_code == SSL_ERROR_WANT_WRITE ){
                return false;
            } else {
                m_result = std::unexpected(CommonError(CallSSLHandshakeError, static_cast<uint32_t>(errno)));
            }
        }
        return true;
    }

    SslRecvEvent::SslRecvEvent(SSL* ssl, EventScheduler* scheduler, char* result, size_t length)
        : SslEvent<std::expected<Bytes, CommonError>>(ssl, scheduler), m_length(length), m_result_str(result)
    {
    }

    void SslRecvEvent::handleEvent()
    {
        sslRecv();
        m_waker.wakeUp();
    }

    bool SslRecvEvent::onReady()
    {
        return sslRecv();
    }

    bool SslRecvEvent::sslRecv()
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
        sslSend();
        m_waker.wakeUp();
    }

    bool SslSendEvent::onReady()
    {
        return sslSend();
    }

    bool SslSendEvent::sslSend()
    {
        using namespace error;
        int sendBytes = SSL_write(m_ssl, m_bytes.data(), m_bytes.size());
        if(sendBytes > 0) LogTrace("sendBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else if (sendBytes == 0) {
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                return false;
            }
            m_result = std::unexpected(CommonError(CallSendError, static_cast<uint32_t>(errno)));
        }
        return true;
    }
}