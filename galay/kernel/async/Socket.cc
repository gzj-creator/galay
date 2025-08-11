#include "Socket.h"

#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/kernel/event/Event.h"

namespace galay {

    SockAddr SockAddrToTHost(const sockaddr* addr) {
        SockAddr host;

        if (!addr) return host;

        char ip_buffer[INET6_ADDRSTRLEN];

        switch(addr->sa_family) {
            case AF_INET: {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(addr);
                inet_ntop(AF_INET, &sin->sin_addr, ip_buffer, sizeof(ip_buffer));
                host.type = IPV4;
                host.host.ip = ip_buffer;
                host.host.port = ntohs(sin->sin_port);
                break;
            }
            case AF_INET6: {
                const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(addr);
                inet_ntop(AF_INET6, &sin6->sin6_addr, ip_buffer, sizeof(ip_buffer));
                host.type = IPV6;
                host.host.ip = ip_buffer;
                host.host.port = ntohs(sin6->sin6_port);
                break;
            }
            default:
            break;
        }
        return host;
    }

    AsyncTcpSocket::AsyncTcpSocket(Runtime& runtime)
        :m_ctx{}
    {
        m_ctx.m_scheduler = runtime.eventScheduler();
    }

    AsyncTcpSocket::AsyncTcpSocket(Runtime& runtime, GHandle handle)
        :m_ctx{}
    {
        m_ctx.m_handle = handle;
        m_ctx.m_scheduler = runtime.eventScheduler();
    }

    AsyncTcpSocket::AsyncTcpSocket(EventScheduler* scheduler, GHandle handle)
        :m_ctx{}
    {
        m_ctx.m_scheduler = scheduler;
        m_ctx.m_handle = handle;
    }


    HandleOption AsyncTcpSocket::options()
    {
        return HandleOption(m_ctx.m_handle);
    }

    ValueWrapper<bool> AsyncTcpSocket::socket()
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        m_ctx.m_handle.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_ctx.m_handle.fd < 0) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSocketError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        HandleOption option(m_ctx.m_handle);
        option.handleNonBlock();
        error = option.getError();
        if(error != nullptr) {
            ::close(m_ctx.m_handle.fd);
            m_ctx.m_handle.fd = -1;
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    ValueWrapper<bool> AsyncTcpSocket::bind(const Host& addr)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty()) addr_in.sin_addr.s_addr = INADDR_ANY;
        else addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::bind(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(addr_in)))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallBindError, errno);
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }


    ValueWrapper<bool> AsyncTcpSocket::listen(int backlog)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        if(::listen(m_ctx.m_handle.fd, backlog))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallListenError, errno);
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    AsyncResult<ValueWrapper<bool>> AsyncTcpSocket::close()
    {
        return {std::make_shared<details::TcpCloseEvent>(m_ctx)};
    }

    AsyncResult<ValueWrapper<AsyncTcpSocketBuilder>> AsyncTcpSocket::accept()
    {
        return {std::make_shared<details::TcpAcceptEvent>(m_ctx)};
    }

    AsyncResult<ValueWrapper<bool>> AsyncTcpSocket::connect(const Host& host)
    {
        return {std::make_shared<details::TcpConnectEvent>(m_ctx, host)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncTcpSocket::recv(size_t length)
    {
        return {std::make_shared<details::TcpRecvEvent>(m_ctx, length)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncTcpSocket::send(Bytes bytes)
    {
        return {std::make_shared<details::TcpSendEvent>(m_ctx, std::move(bytes))};
    }

#ifdef __linux__
    AsyncResult<ValueWrapper<bool>> AsyncTcpSocket::sendfile(GHandle file_handle, long offset, size_t length)
    {
        return {std::make_shared<details::TcpSendfileEvent>(m_ctx, file_handle, offset, length)};
    }
#endif

    ValueWrapper<SockAddr> AsyncTcpSocket::getSrcAddr() const
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        if (getsockname(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetSockNameError, errno);
            makeValue(wrapper, SockAddr(), error);
            return wrapper;
        }
        makeValue(wrapper, SockAddrToTHost(reinterpret_cast<sockaddr*>(&addr)), error);
        return wrapper;
    }

    ValueWrapper<SockAddr> AsyncTcpSocket::getDestAddr() const
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);

        if (getpeername(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetPeerNameError, errno);
            makeValue(wrapper, SockAddr(), error);
            return wrapper;
        }
        makeValue(wrapper, SockAddrToTHost(reinterpret_cast<sockaddr*>(&addr)), error);
        return wrapper;
    }

   
    AsyncUdpSocket::AsyncUdpSocket(Runtime& runtime)
        :m_ctx{}
    {
        m_ctx.m_scheduler = runtime.eventScheduler();
    }

    AsyncUdpSocket::AsyncUdpSocket(Runtime& runtime, GHandle handle)
        :m_ctx{}
    {
        m_ctx.m_handle = handle;
        m_ctx.m_scheduler = runtime.eventScheduler();
    }

    ValueWrapper<bool> AsyncUdpSocket::socket()
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        SystemError::ptr error = nullptr;
        bool success = true;
        m_ctx.m_handle.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_ctx.m_handle.fd < 0) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSocketError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        HandleOption option(m_ctx.m_handle);
        option.handleNonBlock();
        error = option.getError();
        if(error != nullptr) {
            ::close(m_ctx.m_handle.fd);
            m_ctx.m_handle.fd = -1;
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }


    ValueWrapper<bool> AsyncUdpSocket::bind(const Host& addr)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty()) addr_in.sin_addr.s_addr = INADDR_ANY;
        else addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::bind(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallBindError, errno);
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    ValueWrapper<bool> AsyncUdpSocket::connect(const Host& addr)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty())
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallConnectError, errno);
            success = false;
            goto end;
        }
        addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::connect(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallConnectError, errno);
            success = false;
        }
        end:
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncUdpSocket::recvfrom(Host& remote, size_t length)
    {
        return {std::make_shared<details::UdpRecvfromEvent>(m_ctx, remote, length)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncUdpSocket::sendto(const Host& remote, Bytes bytes)
    {
        return {std::make_shared<details::UdpSendtoEvent>(m_ctx, remote, std::move(bytes))};
    }

    AsyncResult<ValueWrapper<bool>> AsyncUdpSocket::close()
    {
        return {std::make_shared<details::UdpCloseEvent>(m_ctx)};
    }

    ValueWrapper<SockAddr> AsyncUdpSocket::getSrcAddr() const
    {
        using namespace error;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        Error::ptr error = nullptr;
        SockAddr saddr {};
        if (getsockname(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetSockNameError, errno);
        } else {
            saddr = SockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
        }
        makeValue(wrapper, std::move(saddr), error);
        return wrapper;
    }

    ValueWrapper<SockAddr> AsyncUdpSocket::getDestAddr() const
    {
         using namespace error;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        Error::ptr error = nullptr;
        SockAddr saddr {};
        if (getpeername(m_ctx.m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetPeerNameError, errno);
        } else {
            saddr = SockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
        }
        makeValue(wrapper, std::move(saddr), error);
        return wrapper;
    }

    AsyncSslSocket::AsyncSslSocket(Runtime& runtime)
        :m_ctx{}
    {
        m_ctx.m_scheduler = runtime.eventScheduler();
    }

    AsyncSslSocket::AsyncSslSocket(Runtime& runtime, GHandle handle, SSL *ssl)
        :m_ctx{}
    {
        m_ctx.m_ssl = ssl;
        m_ctx.m_handle = handle;
        m_ctx.m_scheduler = runtime.eventScheduler();
    }

    AsyncSslSocket::AsyncSslSocket(EventScheduler* scheduler, GHandle handle, SSL* ssl)
        :m_ctx{}
    {
        m_ctx.m_ssl = ssl;
        m_ctx.m_handle = handle;
        m_ctx.m_scheduler = scheduler;
    }

    HandleOption AsyncSslSocket::options()
    {
        return HandleOption(getHandle());
    }

    GHandle AsyncSslSocket::getHandle() const
    {
        return { SSL_get_fd(m_ctx.m_ssl) };
    }

    ValueWrapper<bool> AsyncSslSocket::socket()
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        GHandle handle;
        handle.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (handle.fd < 0) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSocketError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        HandleOption option(handle);
        option.handleNonBlock();
        error = option.getError();
        if(error != nullptr) {
            ::close(handle.fd);
            handle.fd = -1;
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        m_ctx.m_ssl = SSL_new(getGlobalSSLCtx());
        if(m_ctx.m_ssl == nullptr) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSSLNewError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        SSL_set_fd(m_ctx.m_ssl, handle.fd);
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }


    ValueWrapper<bool> AsyncSslSocket::bind(const Host& addr)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty()) addr_in.sin_addr.s_addr = INADDR_ANY;
        else addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::bind(getHandle().fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallBindError, errno);
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    ValueWrapper<bool> AsyncSslSocket::listen(int backlog)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        if(::listen(getHandle().fd, backlog))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallListenError, errno);
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    AsyncResult<ValueWrapper<AsyncSslSocketBuilder>> AsyncSslSocket::sslAccept()
    {
        return {std::make_shared<details::SslAcceptEvent>(m_ctx)};
    }

    AsyncResult<ValueWrapper<bool>> AsyncSslSocket::sslConnect(const Host &addr)
    {
        return {std::make_shared<details::SslConnectEvent>(m_ctx, addr)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncSslSocket::sslRecv(size_t length)
    {
        return {std::make_shared<details::SslRecvEvent>(m_ctx, length)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncSslSocket::sslSend(Bytes bytes)
    {
        return {std::make_shared<details::SslSendEvent>(m_ctx, std::move(bytes))};
    }

    AsyncResult<ValueWrapper<bool>> AsyncSslSocket::sslClose()
    {
        return {std::make_shared<details::SslCloseEvent>(m_ctx)};
    }

} // namespace galay