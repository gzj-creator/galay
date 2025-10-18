#include "NetEvent.h"
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include "Socket.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/common/Log.h"

// macOS上MSG_NOSIGNAL不可用，定义为0
// macOS通过SO_NOSIGPIPE socket选项来防止SIGPIPE
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif


galay::AsyncTcpSocket galay::AsyncTcpSocketBuilder::build()
{
    if(m_handle.fd < 0) {
        LogError("handle < 0");
    }
    
    // macOS上设置SO_NOSIGPIPE选项防止SIGPIPE信号
    #ifdef SO_NOSIGPIPE
    int set = 1;
    if(setsockopt(m_handle.fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set)) < 0) {
        LogWarn("Failed to set SO_NOSIGPIPE: {}", strerror(errno));
    }
    #endif
    
    HandleOption option(m_handle);
    option.handleNonBlock();
    return AsyncTcpSocket(m_scheduler, m_handle);
}

bool galay::AsyncTcpSocketBuilder::check() const
{
    return m_handle.fd >= 0;
}



namespace galay::details
{

    bool AcceptEvent::onReady()
    {
        m_ready = acceptSocket(false);
        return m_ready;
    }

    std::expected<void, CommonError> AcceptEvent::onResume()
    {
        if(!m_ready) acceptSocket(true);
        return AsyncEvent<std::expected<void, CommonError>>::onResume();
    }
    
    bool AcceptEvent::acceptSocket(bool notify)
    {
        using namespace error;
        //accept
        sockaddr addr{};
        socklen_t addr_len = sizeof(addr);
        GHandle handle {
            .fd = accept(m_handle.fd, &addr, &addr_len),
        };
        if( handle.fd < 0 ) {
            if( static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR ) {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallAcceptError, static_cast<uint32_t>(errno)));
            return true;
        }
        std::string ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
        uint16_t port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        LogTrace("[Accept Address: {}:{}]", ip, port);
        m_result = {};
        m_accept_handle = handle;
        return true;
    }

    RecvEvent::RecvEvent(GHandle handle, EventScheduler* scheduler, char* result, size_t length)
        : NetEvent<std::expected<Bytes, CommonError>>(handle, scheduler), m_length(length), m_result_str(result)
    {
    }

    std::expected<Bytes, CommonError> RecvEvent::onResume()
    {
        if(!m_ready) recvBytes(true);
        return NetEvent<std::expected<Bytes, CommonError>>::onResume();
    }

    bool RecvEvent::onReady()
    {
        m_ready = recvBytes(false);
        return m_ready;
    }

    bool RecvEvent::recvBytes(bool notify)
    {
        using namespace error;
        Bytes bytes;
        int recvBytes = recv(m_handle.fd, m_result_str, m_length, 0);
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

    SendEvent::SendEvent(GHandle handle, EventScheduler* scheduler, Bytes &&bytes)
        : NetEvent<std::expected<Bytes, CommonError>>(handle, scheduler), m_bytes(std::move(bytes))
    {
    }

    bool SendEvent::onReady()
    {
        m_ready = sendBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> SendEvent::onResume()
    {
        if(!m_ready) sendBytes(true);
        return AsyncEvent<std::expected<Bytes, CommonError>>::onResume();
    }

    bool SendEvent::sendBytes(bool notify)
    {
        using namespace error;
        // 使用MSG_NOSIGNAL防止SIGPIPE信号
        int sendBytes = send(m_handle.fd, m_bytes.data(), m_bytes.size(), MSG_NOSIGNAL);
        if(sendBytes > 0) LogTrace("sendBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else if (sendBytes == 0) {
            m_result = Bytes{};
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

#ifdef __linux__
    bool SendfileEvent::onReady()
    {
        m_ready = sendfile(false);
        return m_ready;
    }

    bool SendfileEvent::sendfile(bool notify)
    {
        using namespace error;
        int sendBytes = ::sendfile(m_handle.fd, m_file_handle.fd, &m_offset, m_length);
        if (sendBytes > 0) {
            LogTrace("sendfileBytes: {}", sendBytes);
            m_length -= sendBytes;
            m_result = sendBytes;
        } else if (sendBytes == 0) {
            m_result = 0L;
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
            m_result = std::unexpected(CommonError(CallSendfileError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

    std::expected<long, CommonError> SendfileEvent::onResume()
    {
        if(!m_ready) sendfile(true);
        return AsyncEvent<std::expected<long, CommonError>>::onResume();
    }
#endif

    ConnectEvent::ConnectEvent(GHandle handle, EventScheduler* scheduler, const Host &host)
        : NetEvent<std::expected<void, CommonError>>(handle, scheduler), m_host(host)
    {
    }

    bool ConnectEvent::onReady()
    {
        m_ready = connectToHost(false);
        return m_ready;     
    }

    std::expected<void, CommonError> ConnectEvent::onResume()
    {
        if(!m_ready) connectToHost(true);
        return NetEvent<std::expected<void, CommonError>>::onResume();
    }

    bool ConnectEvent::connectToHost(bool notify)
    {
        using namespace error;
        sockaddr_in addr{};
        if( notify ) {
            int error_code = 0;
            socklen_t error_len = sizeof(error_code);
            int ret = getsockopt(m_handle.fd, SOL_SOCKET, SO_ERROR, &error_code, &error_len);
            if (ret < 0 || error_code != 0) {
                m_result = std::unexpected(CommonError(CallConnectError, error_code));
                return true;
            }
            m_result = {};
            return true;
        }
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_host.ip.c_str());
        addr.sin_port = htons(m_host.port);
        const int ret = connect(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_in));
        if( ret != 0) {
            if( static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR || static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EINPROGRESS) {
                return false;
            }
            m_result = std::unexpected(CommonError(CallConnectError, static_cast<uint32_t>(errno)));
            return true;
        }
        m_result = {};
        return true;
    }

    CloseEvent::CloseEvent(GHandle handle, EventScheduler* scheduler)
        : NetEvent<std::expected<void, CommonError>>(handle, scheduler)
    {
    }

    bool CloseEvent::onReady()
    {
        using namespace error;
        m_scheduler->removeEvent(this, nullptr);
        if(::close(m_handle.fd))
        {
            m_result = std::unexpected(CommonError(CallCloseError, static_cast<uint32_t>(errno)));
        } else {
            m_result = {};
        }
        return true;
    }

    bool RecvfromEvent::onReady()
    {
        m_ready = recvfromBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> RecvfromEvent::onResume()
    {
        if(!m_ready) recvfromBytes(true);
        return NetEvent<std::expected<Bytes, CommonError>>::onResume();
    }

    bool RecvfromEvent::recvfromBytes(bool notify)
    {
        using namespace error;
        sockaddr addr;
        socklen_t addr_len = sizeof(addr);
        int recvBytes = recvfrom(m_handle.fd, m_buffer, m_length, 0, &addr, &addr_len);
        if(recvBytes > 0) LogTrace("recvfromBytes: {}, buffer: {}", recvBytes, m_buffer);
        if (recvBytes > 0) {
            Bytes bytes = Bytes::fromCString(m_buffer, recvBytes, recvBytes);
            m_remote.ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
            m_remote.port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
            m_result = std::move(bytes);
        } else if (recvBytes == 0) {
            m_result = Bytes();
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallRecvfromError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

    bool SendtoEvent::onReady()
    {
        m_ready = sendtoBytes(false);;
        return m_ready;
    }

    std::expected<Bytes, CommonError> SendtoEvent::onResume()
    {
        if(!m_ready) sendtoBytes(true);
        return NetEvent<std::expected<Bytes, CommonError>>::onResume();
    }

    bool SendtoEvent::sendtoBytes(bool notify)
    {
        using namespace error;
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_remote.ip.c_str());
        addr.sin_port = htons(m_remote.port);
        // 使用MSG_NOSIGNAL防止SIGPIPE信号
        int sendBytes = sendto(m_handle.fd, m_bytes.data(), m_bytes.size(), MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr));
        if(sendBytes > 0) LogTrace("sendToBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
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
            m_result = std::unexpected(CommonError(CallSendtoError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

}