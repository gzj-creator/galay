#include "Socket.h"

#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "galay/kernel/event/Event.h"

namespace galay {

    SockAddr sockAddrToTHost(const sockaddr* addr) {
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
    {
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    AsyncTcpSocket::AsyncTcpSocket(Runtime& runtime, GHandle handle)
    {
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    AsyncTcpSocket::AsyncTcpSocket(const AsyncTcpSocket& other)
    {
        m_handle = other.m_handle;
        m_scheduler = other.m_scheduler;
        m_buffer = deepCopyString(other.m_buffer);
    }

    AsyncTcpSocket::AsyncTcpSocket(AsyncTcpSocket&& other)
    {
        m_handle = other.m_handle;
        other.m_handle = GHandle::invalid();
        m_scheduler = other.m_scheduler;
        other.m_scheduler = nullptr;
        m_buffer = std::move(other.m_buffer);
    }

    AsyncTcpSocket& AsyncTcpSocket::operator=(const AsyncTcpSocket& other)
    {
        if (this != &other) {
            m_handle = other.m_handle;
            m_scheduler = other.m_scheduler;
            m_buffer = deepCopyString(other.m_buffer);
        }
        return *this;
    }

    AsyncTcpSocket& AsyncTcpSocket::operator=(AsyncTcpSocket&& other)
    {
        if (this != &other) {
            m_handle = other.m_handle;
            m_scheduler = other.m_scheduler;
            m_buffer = std::move(other.m_buffer);
        }
        return *this;
    }

    AsyncTcpSocket::~AsyncTcpSocket()
    {
        freeString(m_buffer);
    }

    AsyncTcpSocket::AsyncTcpSocket(EventScheduler* scheduler, GHandle handle)
    {
        m_scheduler = scheduler;
        m_handle = handle;
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }


    HandleOption AsyncTcpSocket::options()
    {
        return HandleOption(m_handle);
    }

    ValueWrapper<bool> AsyncTcpSocket::socket()
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        Error::ptr error = nullptr;
        bool success = true;
        m_handle.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_handle.fd < 0) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSocketError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        HandleOption option(m_handle);
        option.handleNonBlock();
        error = option.getError();
        if(error != nullptr) {
            ::close(m_handle.fd);
            m_handle.fd = -1;
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
        if(::bind(m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(addr_in)))
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
        if(::listen(m_handle.fd, backlog))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallListenError, errno);
            success = false;
        }
        makeValue(wrapper, std::move(success), error);
        return wrapper;
    }

    AsyncResult<ValueWrapper<bool>> AsyncTcpSocket::close()
    {
        return {std::make_shared<details::TcpCloseEvent>(m_handle, m_scheduler)};
    }

    AsyncResult<ValueWrapper<AsyncTcpSocketBuilder>> AsyncTcpSocket::accept()
    {
        return {std::make_shared<details::TcpAcceptEvent>(m_handle, m_scheduler)};
    }

    AsyncResult<ValueWrapper<bool>> AsyncTcpSocket::connect(const Host& host)
    {
        return {std::make_shared<details::TcpConnectEvent>(m_handle, m_scheduler, host)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncTcpSocket::recv(size_t length)
    {
        if(m_buffer.capacity < length) {
            reallocString(m_buffer, length);
        }
        clearString(m_buffer);
        return {std::make_shared<details::TcpRecvEvent>(m_handle, m_scheduler, reinterpret_cast<char*>(m_buffer.data), length)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncTcpSocket::send(Bytes bytes)
    {
        return {std::make_shared<details::TcpSendEvent>(m_handle, m_scheduler, std::move(bytes))};
    }

#ifdef __linux__
    AsyncResult<ValueWrapper<long>> AsyncTcpSocket::sendfile(GHandle file_handle, long offset, size_t length)
    {
        return {std::make_shared<details::TcpSendfileEvent>(m_handle, m_scheduler, file_handle, offset, length)};
    }
#endif
    void AsyncTcpSocket::reallocBuffer(size_t length)
    {
        reallocString(m_buffer, length);
    }

    ValueWrapper<SockAddr> AsyncTcpSocket::getSrcAddr() const
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        if (getsockname(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetSockNameError, errno);
            makeValue(wrapper, SockAddr(), error);
            return wrapper;
        }
        makeValue(wrapper, sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr)), error);
        return wrapper;
    }

    ValueWrapper<SockAddr> AsyncTcpSocket::getDestAddr() const
    {
        using namespace error;
        SystemError::ptr error = nullptr;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);

        if (getpeername(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetPeerNameError, errno);
            makeValue(wrapper, SockAddr(), error);
            return wrapper;
        }
        makeValue(wrapper, sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr)), error);
        return wrapper;
    }

   
    AsyncUdpSocket::AsyncUdpSocket(Runtime& runtime)
    {
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    AsyncUdpSocket::AsyncUdpSocket(Runtime& runtime, GHandle handle)
    {
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    AsyncUdpSocket::AsyncUdpSocket(const AsyncUdpSocket& other)
    {
        m_handle = other.m_handle;
        m_scheduler = other.m_scheduler;
        m_buffer = deepCopyString(other.m_buffer);
    }

    AsyncUdpSocket::AsyncUdpSocket(AsyncUdpSocket&& other)
    {
        m_handle = other.m_handle;
        other.m_handle = GHandle::invalid();
        m_scheduler = other.m_scheduler;
        other.m_scheduler = nullptr;
        m_buffer = std::move(other.m_buffer);
    }
    
    AsyncUdpSocket& AsyncUdpSocket::operator=(const AsyncUdpSocket& other)
    {
        if(this != &other) {
            m_handle = other.m_handle;
            m_scheduler = other.m_scheduler;
            m_buffer = deepCopyString(other.m_buffer);
        }
        return *this;
    }

    AsyncUdpSocket& AsyncUdpSocket::operator=(AsyncUdpSocket&& other)
    {
        if(this != &other) {
            m_handle = other.m_handle;
            other.m_handle = GHandle::invalid();
            m_scheduler = other.m_scheduler;
            other.m_scheduler = nullptr;
            m_buffer = std::move(other.m_buffer);
        }
        return *this;
    }

    AsyncUdpSocket::~AsyncUdpSocket()
    {
        freeString(m_buffer);
    }

    HandleOption AsyncUdpSocket::options()
    {
        return HandleOption(m_handle);
    }

    ValueWrapper<bool> AsyncUdpSocket::socket()
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        SystemError::ptr error = nullptr;
        bool success = true;
        m_handle.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_handle.fd < 0) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSocketError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        HandleOption option(m_handle);
        option.handleNonBlock();
        error = option.getError();
        if(error != nullptr) {
            ::close(m_handle.fd);
            m_handle.fd = -1;
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
        if(::bind(m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
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
        if(::connect(m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
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
        if(m_buffer.capacity < length) {
            reallocString(m_buffer, length);
        } 
        clearString(m_buffer);
        return {std::make_shared<details::UdpRecvfromEvent>(m_handle, m_scheduler, remote, reinterpret_cast<char*>(m_buffer.data), length)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncUdpSocket::sendto(const Host& remote, Bytes bytes)
    {
        return {std::make_shared<details::UdpSendtoEvent>(m_handle, m_scheduler, remote, std::move(bytes))};
    }

    AsyncResult<ValueWrapper<bool>> AsyncUdpSocket::close()
    {
        return {std::make_shared<details::UdpCloseEvent>(m_handle, m_scheduler)};
    }

    void AsyncUdpSocket::reallocBuffer(size_t length)
    {
        reallocString(m_buffer, length);
    }

    ValueWrapper<SockAddr> AsyncUdpSocket::getSrcAddr() const
    {
        using namespace error;
        ValueWrapper<SockAddr> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        Error::ptr error = nullptr;
        SockAddr saddr {};
        if (getsockname(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetSockNameError, errno);
        } else {
            saddr = sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
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
        if (getpeername(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = std::make_shared<SystemError>(error::CallGetPeerNameError, errno);
        } else {
            saddr = sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
        }
        makeValue(wrapper, std::move(saddr), error);
        return wrapper;
    }

    AsyncSslSocket::AsyncSslSocket(Runtime& runtime)
    {
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    AsyncSslSocket::AsyncSslSocket(Runtime& runtime, SSL *ssl)
    {
        m_ssl = ssl;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    AsyncSslSocket::AsyncSslSocket(AsyncSslSocket &&other)
    {
        m_ssl = other.m_ssl;
        other.m_ssl = nullptr;
        m_scheduler = other.m_scheduler;
        other.m_scheduler = nullptr;
        m_buffer = std::move(other.m_buffer);
    }

    AsyncSslSocket::AsyncSslSocket(const AsyncSslSocket &other)
    {
        m_ssl = other.m_ssl;
        m_scheduler = other.m_scheduler;
        m_buffer = deepCopyString(other.m_buffer);
    }

    AsyncSslSocket &AsyncSslSocket::operator=(const AsyncSslSocket &other)
    {
        if(this == &other) {
            m_ssl = other.m_ssl;
            m_scheduler = other.m_scheduler;
            m_buffer = deepCopyString(other.m_buffer);
        }
        return *this;
    }

    AsyncSslSocket &AsyncSslSocket::operator=(AsyncSslSocket &&other)
    {
        if(this != &other) {
            m_ssl = other.m_ssl;
            other.m_ssl = nullptr;
            m_scheduler = other.m_scheduler;
            other.m_scheduler = nullptr;
            m_buffer = std::move(other.m_buffer);
        }
        return *this;
    }

    AsyncSslSocket::AsyncSslSocket(EventScheduler* scheduler, SSL* ssl)
    {
        m_ssl = ssl;
        m_scheduler = scheduler;
        m_buffer = mallocString(DEFAULT_BUFFER_SIZE);
    }

    HandleOption AsyncSslSocket::options()
    {
        return HandleOption(getHandle());
    }

    GHandle AsyncSslSocket::getHandle() const
    {
        return { SSL_get_fd(m_ssl) };
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
        m_ssl = SSL_new(getGlobalSSLCtx());
        if(m_ssl == nullptr) {
            error = std::make_shared<SystemError>(error::ErrorCode::CallSSLNewError, errno);
            success = false;
            makeValue(wrapper, std::move(success), error);
            return wrapper;
        }
        SSL_set_fd(m_ssl, handle.fd);
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
        return {std::make_shared<details::SslAcceptEvent>(m_ssl, m_scheduler)};
    }

    AsyncResult<ValueWrapper<bool>> AsyncSslSocket::sslConnect(const Host &addr)
    {
        return {std::make_shared<details::SslConnectEvent>(m_ssl, m_scheduler, addr)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncSslSocket::sslRecv(size_t length)
    {
        if(m_buffer.capacity < length) {
            reallocString(m_buffer, length);
        }
        clearString(m_buffer);
        return {std::make_shared<details::SslRecvEvent>(m_ssl, m_scheduler, reinterpret_cast<char*>(m_buffer.data), length)};
    }

    AsyncResult<ValueWrapper<Bytes>> AsyncSslSocket::sslSend(Bytes bytes)
    {
        return {std::make_shared<details::SslSendEvent>(m_ssl, m_scheduler, std::move(bytes))};
    }

    AsyncResult<ValueWrapper<bool>> AsyncSslSocket::sslClose()
    {
        return {std::make_shared<details::SslCloseEvent>(m_ssl, m_scheduler)};
    }

    void AsyncSslSocket::reallocBuffer(size_t length)
    {
        reallocString(m_buffer, length);
    }

    AsyncSslSocket::~AsyncSslSocket()
    {
        freeString(m_buffer);
    }

} // namespace galay