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

    ValueWrapper<AsyncTcpSocketBuilder> AcceptEvent::resume()
    {
        if(!m_ready) acceptSocket(true);
        return AsyncEvent<ValueWrapper<AsyncTcpSocketBuilder>>::resume();
    }
    
    bool AcceptEvent::acceptSocket(bool notify)
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        //accept
        sockaddr addr{};
        socklen_t addr_len = sizeof(addr);
        GHandle handle {
            .fd = accept(m_handle.fd, &addr, &addr_len),
        };
        if( handle.fd < 0 ) {
            if( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
                if( notify ) {
                    error = std::make_shared<SystemError>(NotifyButSourceNotReadyError, errno);
                    makeValue(m_result, AsyncTcpSocketBuilder::create(m_scheduler, handle), error);
                }
                return false;
            }
            error = std::make_shared<SystemError>(CallAcceptError, errno);
        }
        std::string ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
        uint16_t port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        LogTrace("[Accept Address: {}:{}]", ip, port);
        makeValue(m_result, AsyncTcpSocketBuilder::create(m_scheduler, handle), error);
        return true;
    }

    RecvEvent::RecvEvent(GHandle handle, EventScheduler* scheduler, char* buffer, size_t length)
        : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_length(length), m_buffer(buffer)
    {
    }

    ValueWrapper<Bytes> RecvEvent::resume()
    {
        if(!m_ready) recvBytes(true);
        return NetEvent<ValueWrapper<Bytes>>::resume();
    }

    bool RecvEvent::ready()
    {
        m_ready = recvBytes(false);
        return m_ready;
    }

    bool RecvEvent::recvBytes(bool notify)
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        Bytes bytes;
        int recvBytes = recv(m_handle.fd, m_buffer, m_length, 0);
        LogTrace("recvBytes: {}, buffer: {}", recvBytes, m_buffer);
        if (recvBytes > 0) {
            bytes = Bytes::fromCString(m_buffer, recvBytes, m_length);
        } else if (recvBytes == 0) {
            error = std::make_shared<SystemError>(DisConnectError, errno);
            bytes = Bytes();
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                if( notify ) {
                    error = std::make_shared<SystemError>(NotifyButSourceNotReadyError, errno);
                    makeValue(m_result, Bytes(), error);
                }
                return false;
            }
            error = std::make_shared<SystemError>(CallRecvError, errno);
            bytes = Bytes();
        }
        makeValue(m_result, std::move(bytes), error);
        return true;
    }

    SendEvent::SendEvent(GHandle handle, EventScheduler* scheduler, Bytes &&bytes)
        : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_bytes(std::move(bytes))
    {
    }

    bool SendEvent::ready()
    {
        m_ready = sendBytes(false);
        return m_ready;
    }

    ValueWrapper<Bytes> SendEvent::resume()
    {
        if(!m_ready) sendBytes(true);
        return AsyncEvent<ValueWrapper<Bytes>>::resume();
    }

    bool SendEvent::sendBytes(bool notify)
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        int sendBytes = send(m_handle.fd, m_bytes.data(), m_bytes.size(), 0);
        LogTrace("sendBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            makeValue(m_result, std::move(remain), error);
        } else if (sendBytes == 0) {
            error = std::make_shared<SystemError>(DisConnectError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                if( notify ) {
                    error = std::make_shared<SystemError>(NotifyButSourceNotReadyError, errno);
                    makeValue(m_result, std::move(m_bytes), error);
                }
                return false;
            }
            error = std::make_shared<SystemError>(CallSendError, errno);
            makeValue(m_result, std::move(m_bytes), error);
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
        SystemError::ptr error = nullptr;
        while(m_length > 0) {
            int sendBytes = ::sendfile(m_handle.fd, m_file_handle.fd, &m_offset, m_length);
            if (sendBytes < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
                {
                    if( notify ) {
                        error = std::make_shared<SystemError>(NotifyButSourceNotReadyError, errno);
                        makeValue(m_result, std::move(m_offset), error);
                    }
                    return false;
                }
                error = std::make_shared<SystemError>(CallSendfileError, errno);
                makeValue(m_result, std::move(m_offset), error);
            }
            m_length -= sendBytes;
        }
        return true;
    }

    ValueWrapper<long> SendfileEvent::resume()
    {
        if(!m_ready) sendfile(true);
        return AsyncEvent<ValueWrapper<long>>::resume();
    }
#endif

    ConnectEvent::ConnectEvent(GHandle handle, EventScheduler* scheduler, const Host &host)
        : NetEvent<ValueWrapper<bool>>(handle, scheduler), m_host(host)
    {
    }

    bool ConnectEvent::ready()
    {
        m_ready = connectToHost(false);
        return m_ready;     
    }

    ValueWrapper<bool> ConnectEvent::resume()
    {
        if(!m_ready) connectToHost(true);
        return NetEvent<ValueWrapper<bool>>::resume();
    }

    bool ConnectEvent::connectToHost(bool notify)
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        bool success = true;
        sockaddr_in addr{};
        if( notify ) {
            makeValue(m_result, true, error);
            return true;
        }
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_host.ip.c_str());
        addr.sin_port = htons(m_host.port);
        const int ret = connect(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr_in));
        if( ret != 0) {
            if( errno == EWOULDBLOCK || errno == EINTR || errno == EAGAIN || errno == EINPROGRESS) {
                return false;
            }
            success = false;
            error = std::make_shared<SystemError>(CallConnectError, errno);
        }
        makeValue(m_result, std::move(success), error);
        return true;
    }

    CloseEvent::CloseEvent(GHandle handle, EventScheduler* scheduler)
        : NetEvent<ValueWrapper<bool>>(handle, scheduler)
    {
    }

    bool CloseEvent::ready()
    {
        using namespace error;
        Error::ptr error = nullptr;
        bool success = true;
        m_scheduler->removeEvent(this, nullptr);
        if(::close(m_handle.fd))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallCloseError, errno);
            success = false;
        } 
        makeValue(m_result, std::move(success), error);
        return true;
    }

    bool RecvfromEvent::ready()
    {
        m_ready = recvfromBytes(false);
        return m_ready;
    }

    ValueWrapper<Bytes> RecvfromEvent::resume()
    {
        if(!m_ready) recvfromBytes(true);
        return NetEvent<ValueWrapper<Bytes>>::resume();
    }

    bool RecvfromEvent::recvfromBytes(bool notify)
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        Bytes bytes;
        sockaddr addr;
        socklen_t addr_len = sizeof(addr);
        int recvBytes = recvfrom(m_handle.fd, m_buffer, m_length, 0, &addr, &addr_len);
        LogTrace("recvfromBytes: {}, buffer: {}", recvBytes, m_buffer);
        if (recvBytes > 0) {
            bytes = Bytes::fromCString(m_buffer, recvBytes, m_length);
            m_remote.ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
            m_remote.port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        } else if (recvBytes == 0) {
            bytes = Bytes();
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                if( notify ) {
                    error = std::make_shared<SystemError>(NotifyButSourceNotReadyError, errno);
                    makeValue(m_result, Bytes(), error);
                }
                return false;
            }
            error = std::make_shared<SystemError>(CallRecvfromError, errno);
            bytes = Bytes();
        }
        makeValue(m_result, std::move(bytes), error);
        return true;
    }

    bool SendtoEvent::ready()
    {
        m_ready = sendtoBytes(false);;
        return m_ready;
    }

    ValueWrapper<Bytes> SendtoEvent::resume()
    {
        if(!m_ready) sendtoBytes(true);
        return NetEvent<ValueWrapper<Bytes>>::resume();
    }

    bool SendtoEvent::sendtoBytes(bool notify)
    {
        using namespace error;
        SystemError::ptr error;
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_remote.ip.c_str());
        addr.sin_port = htons(m_remote.port);
        int sendBytes = sendto(m_handle.fd, m_bytes.data(), m_bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr));
        LogTrace("sendToBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(m_bytes.data())));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            makeValue(m_result, std::move(remain), error);
        } else if (sendBytes == 0) {
            error = std::make_shared<SystemError>(DisConnectError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                if( notify ) {
                    error = std::make_shared<SystemError>(NotifyButSourceNotReadyError, errno);
                    makeValue(m_result, std::move(m_bytes), error);
                }
                return false;
            }
            error = std::make_shared<SystemError>(CallSendtoError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        }
        return true;
    }

}