#include "NetEvent.h"
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include "Socket.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/common/Log.h"

galay::AsyncTcpSocketBuilder galay::AsyncTcpSocketBuilder::create(EventScheduler* scheduler, GHandle handle)
{
    AsyncTcpSocketBuilder builder;
    builder.m_handle = handle;
    builder.m_scheduler = scheduler;
    return builder;
}


galay::AsyncTcpSocket galay::AsyncTcpSocketBuilder::build()
{
    if(m_handle.fd < 0) {
        LogError("handle < 0");
    }
    return AsyncTcpSocket(m_scheduler, m_handle);
}

bool galay::AsyncTcpSocketBuilder::check() const
{
    return m_handle.fd >= 0;
}



namespace galay::details
{

    bool AcceptEvent::ready()
    {
        m_ready = acceptSocket(false);
        return m_ready;
    }

    std::expected<AsyncTcpSocketBuilder, CommonError> AcceptEvent::resume()
    {
        if(!m_ready) acceptSocket(true);
        return AsyncEvent<std::expected<AsyncTcpSocketBuilder, CommonError>>::resume();
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
        }
        std::string ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
        uint16_t port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        LogTrace("[Accept Address: {}:{}]", ip, port);
        m_result = AsyncTcpSocketBuilder::create(m_scheduler, handle);
        return true;
    }

    RecvEvent::RecvEvent(GHandle handle, EventScheduler* scheduler, char* result, size_t length)
        : NetEvent<std::expected<Bytes, CommonError>>(handle, scheduler), m_length(length), m_result_str(result)
    {
    }

    std::expected<Bytes, CommonError> RecvEvent::resume()
    {
        if(!m_ready) recvBytes(true);
        return NetEvent<std::expected<Bytes, CommonError>>::resume();
    }

    bool RecvEvent::ready()
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

    bool SendEvent::ready()
    {
        m_ready = sendBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> SendEvent::resume()
    {
        if(!m_ready) sendBytes(true);
        return AsyncEvent<std::expected<Bytes, CommonError>>::resume();
    }

    bool SendEvent::sendBytes(bool notify)
    {
        using namespace error;
        int sendBytes = send(m_handle.fd, m_bytes.data(), m_bytes.size(), 0);
        if(sendBytes > 0) LogTrace("sendBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else if (sendBytes == 0) {
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallSendError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

#ifdef __linux__
    bool SendfileEvent::ready()
    {
        m_ready = sendfile(false);
        return m_ready;
    }

    bool SendfileEvent::sendfile(bool notify)
    {
        using namespace error;
        long total = 0;
        while(m_length > 0) {
            int sendBytes = ::sendfile(m_handle.fd, m_file_handle.fd, &m_offset, m_length);
            if (sendBytes < 0) {
                if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
                {
                    if( notify ) {
                        m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                    }
                    return false;
                }
                m_result = std::unexpected(CommonError(CallSendfileError, static_cast<uint32_t>(errno)));
            }
            m_length -= sendBytes;
            total += sendBytes;
        }
        m_result = total;
        return true;
    }

    std::expected<long, CommonError> SendfileEvent::resume()
    {
        if(!m_ready) sendfile(true);
        return AsyncEvent<std::expected<long, CommonError>>::resume();
    }
#endif

    ConnectEvent::ConnectEvent(GHandle handle, EventScheduler* scheduler, const Host &host)
        : NetEvent<std::expected<void, CommonError>>(handle, scheduler), m_host(host)
    {
    }

    bool ConnectEvent::ready()
    {
        m_ready = connectToHost(false);
        return m_ready;     
    }

    std::expected<void, CommonError> ConnectEvent::resume()
    {
        if(!m_ready) connectToHost(true);
        return NetEvent<std::expected<void, CommonError>>::resume();
    }

    bool ConnectEvent::connectToHost(bool notify)
    {
        using namespace error;
        sockaddr_in addr{};
        if( notify ) {
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
        }
        m_result = {};
        return true;
    }

    CloseEvent::CloseEvent(GHandle handle, EventScheduler* scheduler)
        : NetEvent<std::expected<void, CommonError>>(handle, scheduler)
    {
    }

    bool CloseEvent::ready()
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

    bool RecvfromEvent::ready()
    {
        m_ready = recvfromBytes(false);
        return m_ready;
    }

    std::expected<Bytes, CommonError> RecvfromEvent::resume()
    {
        if(!m_ready) recvfromBytes(true);
        return NetEvent<std::expected<Bytes, CommonError>>::resume();
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

    bool SendtoEvent::ready()
    {
        m_ready = sendtoBytes(false);;
        return m_ready;
    }

    std::expected<Bytes, CommonError> SendtoEvent::resume()
    {
        if(!m_ready) sendtoBytes(true);
        return NetEvent<std::expected<Bytes, CommonError>>::resume();
    }

    bool SendtoEvent::sendtoBytes(bool notify)
    {
        using namespace error;
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_remote.ip.c_str());
        addr.sin_port = htons(m_remote.port);
        int sendBytes = sendto(m_handle.fd, m_bytes.data(), m_bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr));
        if(sendBytes > 0) LogTrace("sendToBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else if (sendBytes == 0) {
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
        } else {
            if(static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR )
            {
                if( notify ) {
                    m_result = std::unexpected(CommonError(NotifyButSourceNotReadyError, static_cast<uint32_t>(errno)));
                }
                return false;
            }
            m_result = std::unexpected(CommonError(CallSendtoError, static_cast<uint32_t>(errno)));
        }
        return true;
    }

}