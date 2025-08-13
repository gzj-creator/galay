#include "NetEvent.h"
#ifdef __linux__
#include <sys/sendfile.h>
#endif
#include "Socket.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"

galay::AsyncTcpSocketBuilder galay::AsyncTcpSocketBuilder::create(EventScheduler* scheduler, GHandle handle)
{
    AsyncTcpSocketBuilder builder;
    builder.m_handle = handle;
    builder.m_scheduler = scheduler;
    return builder;
}


galay::AsyncTcpSocket galay::AsyncTcpSocketBuilder::build()
{
    if(m_handle.fd < 0) throw std::runtime_error("Invalid handle");
    return AsyncTcpSocket(m_scheduler, m_handle);
}

bool galay::AsyncTcpSocketBuilder::check() const
{
    return m_handle.fd >= 0;
}



namespace galay::details
{

    bool TcpAcceptEvent::ready()
    {
        return acceptSocket();
    }

    void TcpAcceptEvent::handleEvent()
    {
        acceptSocket();
        m_waker.wakeUp();
    }
    
    bool TcpAcceptEvent::acceptSocket()
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        //accept
        sockaddr addr{};
        socklen_t addr_len = sizeof(addr);
        GHandle handle {
            .fd = accept(m_handle.fd, &addr, &addr_len),
        };
        LogTrace("[Accept Address: {}:{}]", ip, port);
        if( handle.fd < 0 ) {
            if( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
                return false;
            }
            error = std::make_shared<SystemError>(CallAcceptError, errno);
        }
        makeValue(m_result, AsyncTcpSocketBuilder::create(m_scheduler, handle), error);
        return true;
    }

    TcpRecvEvent::TcpRecvEvent(GHandle handle, EventScheduler* scheduler, size_t length)
        : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_length(length)
    {
    }

    void TcpRecvEvent::handleEvent()
    {
        recvBytes();
        m_waker.wakeUp();
    }

    bool TcpRecvEvent::ready()
    {
        return recvBytes();
    }

    bool TcpRecvEvent::recvBytes()
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        Bytes bytes(m_length);
        int recvBytes = recv(m_handle.fd, bytes.data(), m_length, 0);
        if (recvBytes > 0) {
            BytesVisitor visitor(bytes);
            visitor.size() = recvBytes;
        } else if (recvBytes == 0) {
            error = std::make_shared<SystemError>(DisConnectError, errno);
            bytes = Bytes();
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                return false;
            }
            error = std::make_shared<SystemError>(CallRecvError, errno);
            bytes = Bytes();
        }
        makeValue(m_result, std::move(bytes), error);
        return true;
    }

    TcpSendEvent::TcpSendEvent(GHandle handle, EventScheduler* scheduler, Bytes &&bytes)
        : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_bytes(std::move(bytes))
    {
    }

    bool TcpSendEvent::ready()
    {
        return sendBytes();
    }

    void TcpSendEvent::handleEvent()
    {
        sendBytes();
        m_waker.wakeUp();
    }

    bool TcpSendEvent::sendBytes()
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        int sendBytes = send(m_handle.fd, m_bytes.data(), m_bytes.size(), 0);
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            makeValue(m_result, std::move(remain), error);
        } else if (sendBytes == 0) {
            error = std::make_shared<SystemError>(DisConnectError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                return false;
            }
            error = std::make_shared<SystemError>(CallSendError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        }
        return true;
    }

#ifdef __linux__
    bool TcpSendfileEvent::ready()
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        while(m_length > 0) {
            int sendBytes = sendfile(m_handle.fd, m_file_handle.fd, &m_offset, m_length);
            if (sendBytes < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
                {
                    return false;
                }
                error = std::make_shared<SystemError>(CallSendfileError, errno);
                makeValue(m_result, false, error);
            }
            m_length -= sendBytes;
        }
        return true;
    }

    void TcpSendfileEvent::handleEvent()
    {
        using namespace error;
        SystemError::ptr  error = nullptr;
        while(m_length > 0) {
            int sendBytes = sendfile(m_handle.fd, m_file_handle.fd, &m_offset, m_length);
            if (sendBytes < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
                {
                    TcpSendfileEvent::suspend(m_waker);
                    return;
                }
                error = std::make_shared<SystemError>(CallSendfileError, errno);
                makeValue(m_result, false, error);
            }
            m_length -= sendBytes;
        }
        m_waker.wakeUp();
    }
#endif
    bool TcpConnectEvent::ready()
    {
        return connectToHost();     
    }

    TcpConnectEvent::TcpConnectEvent(GHandle handle, EventScheduler* scheduler, const Host &host)
        : NetEvent<ValueWrapper<bool>>(handle, scheduler), m_host(host)
    {
    }

    void TcpConnectEvent::handleEvent()
    {
        m_waker.wakeUp();
    }


    bool TcpConnectEvent::connectToHost()
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        bool success = true;
        sockaddr_in addr{};
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

    TcpCloseEvent::TcpCloseEvent(GHandle handle, EventScheduler* scheduler)
        : NetEvent<ValueWrapper<bool>>(handle, scheduler)
    {
    }

    bool TcpCloseEvent::ready()
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

    void UdpRecvfromEvent::handleEvent()
    {
        recvfromBytes();
        m_waker.wakeUp();
    }

    bool UdpRecvfromEvent::ready()
    {
        return recvfromBytes();
    }

    bool UdpRecvfromEvent::recvfromBytes()
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        Bytes bytes(m_length);
        sockaddr addr;
        socklen_t addr_len = sizeof(addr);
        int recvBytes = recvfrom(m_handle.fd, bytes.data(), m_length, 0, &addr, &addr_len);
        if (recvBytes > 0) {
            BytesVisitor visitor(bytes);
            visitor.size() = recvBytes;
            m_remote.ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
            m_remote.port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
        } else if (recvBytes == 0) {
            bytes = Bytes();
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                return false;
            }
            error = std::make_shared<SystemError>(CallRecvfromError, errno);
            bytes = Bytes();
        }
        makeValue(m_result, std::move(bytes), error);
        return true;
    }

    void UdpSendtoEvent::handleEvent()
    {
        sendtoBytes();
        m_waker.wakeUp();
    }

    bool UdpSendtoEvent::ready()
    {
        return sendtoBytes();
    }

    bool UdpSendtoEvent::sendtoBytes()
    {
        using namespace error;
        SystemError::ptr error;
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(m_remote.ip.c_str());
        addr.sin_port = htons(m_remote.port);
        int sendBytes = sendto(m_handle.fd, m_bytes.data(), m_bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(sockaddr));
        if (sendBytes > 0) {
            Bytes remain(m_bytes.data() + sendBytes, m_bytes.size() - sendBytes);
            makeValue(m_result, std::move(remain), error);
        } else if (sendBytes == 0) {
            error = std::make_shared<SystemError>(DisConnectError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        } else {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            {
                return false;
            }
            error = std::make_shared<SystemError>(CallSendtoError, errno);
            makeValue(m_result, std::move(m_bytes), error);
        }
        return true;
    }

    UdpCloseEvent::UdpCloseEvent(GHandle handle, EventScheduler* scheduler)
        : NetEvent<ValueWrapper<bool>>(handle, scheduler)
    {
    }

    bool UdpCloseEvent::ready()
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
}