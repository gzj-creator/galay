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
    }

    AsyncTcpSocket::AsyncTcpSocket(Runtime& runtime, GHandle handle)
    {
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
        HandleOption options(handle);
        options.handleNonBlock();
    }

    AsyncTcpSocket::AsyncTcpSocket(AsyncTcpSocket&& other)
    {
        m_handle = other.m_handle;
        other.m_handle = GHandle::invalid();
        m_scheduler = other.m_scheduler;
        other.m_scheduler = nullptr;
    }

    AsyncTcpSocket& AsyncTcpSocket::operator=(AsyncTcpSocket&& other)
    {
        if (this != &other) {
            m_handle = other.m_handle;
            m_scheduler = other.m_scheduler;
        }
        return *this;
    }

    AsyncTcpSocket::~AsyncTcpSocket()
    {
    }

    AsyncTcpSocket::AsyncTcpSocket(EventScheduler* scheduler, GHandle handle)
    {
        m_scheduler = scheduler;
        m_handle = handle;
    }


    HandleOption AsyncTcpSocket::options()
    {
        return HandleOption(m_handle);
    }

    std::expected<void, CommonError> AsyncTcpSocket::socket()
    {
        using namespace error;
        m_handle.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_handle.fd < 0) {
            return std::unexpected(CommonError(CallSocketError, static_cast<uint32_t>(errno)));
        }
        
        // macOS上设置SO_NOSIGPIPE选项防止SIGPIPE信号
        #ifdef SO_NOSIGPIPE
        int set = 1;
        (void)setsockopt(m_handle.fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
        #endif
        
        HandleOption option(m_handle);
        auto res = option.handleNonBlock();
        if(!res) {
            ::close(m_handle.fd);
            m_handle.fd = -1;
        }
        return res;
    }

    std::expected<void, CommonError> AsyncTcpSocket::bind(const Host& addr)
    {
        using namespace error;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty()) addr_in.sin_addr.s_addr = INADDR_ANY;
        else addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::bind(m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(addr_in)))
        {
            return std::unexpected(CommonError(CallBindError, static_cast<uint32_t>(errno)));
        }
        return {};
    }


    std::expected<void, CommonError> AsyncTcpSocket::listen(int backlog)
    {
        using namespace error;
        if(::listen(m_handle.fd, backlog))
        {
            return std::unexpected(CommonError(CallListenError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> AsyncTcpSocket::shuntdown(ShutdownType type)
    {
        using namespace error;
        int ret = -1;
        switch (type)
        {
        case ShutdownType::Read:
            ret = ::shutdown(m_handle.fd, SHUT_RD);
            break;
        case ShutdownType::Write:
            ret = ::shutdown(m_handle.fd, SHUT_WR);
            break;
        case ShutdownType::Both:
            ret = ::shutdown(m_handle.fd, SHUT_RDWR);
            break;
        default:
            break;
        }
        if(ret != 0) {
            return std::unexpected(CommonError(CallShuntdownError, static_cast<uint32_t>(errno)));
        } 
        return {};
    }


    AsyncResult<std::expected<void, CommonError>> AsyncTcpSocket::close()
    {
        return {std::make_shared<details::CloseEvent>(m_handle, m_scheduler)};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncTcpSocket::accept(AsyncTcpSocketBuilder& builder)
    {
        builder.m_scheduler = m_scheduler;
        return {std::make_shared<details::AcceptEvent>(m_handle, m_scheduler, builder.m_handle)};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncTcpSocket::connect(const Host& host)
    {
        return {std::make_shared<details::ConnectEvent>(m_handle, m_scheduler, host)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncTcpSocket::recv(char* result, size_t length)
    {
        return {std::make_shared<details::RecvEvent>(m_handle, m_scheduler, result, length)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncTcpSocket::send(Bytes bytes)
    {
        return {std::make_shared<details::SendEvent>(m_handle, m_scheduler, std::move(bytes))};
    }

#ifdef __linux__
    AsyncResult<std::expected<long, CommonError>> AsyncTcpSocket::sendfile(GHandle file_handle, long offset, size_t length)
    {
        return {std::make_shared<details::SendfileEvent>(m_handle, m_scheduler, file_handle, offset, length)};
    }
#endif

    std::expected<SockAddr, CommonError> AsyncTcpSocket::getSrcAddr() const
    {
        using namespace error;
        std::expected<SockAddr, CommonError> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        if (getsockname(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return std::unexpected(CommonError(CallGetSockNameError, static_cast<uint32_t>(errno)));
        }
        return sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
    }

    std::expected<SockAddr, CommonError> AsyncTcpSocket::getDestAddr() const
    {
        using namespace error;
        std::expected<SockAddr, CommonError> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);

        if (getpeername(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return std::unexpected(CommonError(CallGetPeerNameError, static_cast<uint32_t>(errno)));
        }
        return sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
    }

    AsyncTcpSocket AsyncTcpSocket::cloneForDifferentRole(Runtime &runtime) const
    {
        return AsyncTcpSocket(runtime, m_handle);
    }

    AsyncUdpSocket::AsyncUdpSocket(Runtime& runtime)
    {
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    AsyncUdpSocket::AsyncUdpSocket(Runtime& runtime, GHandle handle)
    {
        m_handle = handle;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    AsyncUdpSocket::AsyncUdpSocket(AsyncUdpSocket&& other)
    {
        m_handle = other.m_handle;
        other.m_handle = GHandle::invalid();
        m_scheduler = other.m_scheduler;
        other.m_scheduler = nullptr;
    }

    AsyncUdpSocket& AsyncUdpSocket::operator=(AsyncUdpSocket&& other)
    {
        if(this != &other) {
            m_handle = other.m_handle;
            other.m_handle = GHandle::invalid();
            m_scheduler = other.m_scheduler;
            other.m_scheduler = nullptr;
        }
        return *this;
    }

    AsyncUdpSocket::~AsyncUdpSocket()
    {
    }

    HandleOption AsyncUdpSocket::options()
    {
        return HandleOption(m_handle);
    }

    std::expected<void, CommonError> AsyncUdpSocket::socket()
    {
        using namespace error;
        m_handle.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_handle.fd < 0) {
            return std::unexpected(CommonError(CallSocketError, static_cast<uint32_t>(errno)));
        }
        
        // macOS上设置SO_NOSIGPIPE选项防止SIGPIPE信号
        #ifdef SO_NOSIGPIPE
        int set = 1;
        (void)setsockopt(m_handle.fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
        #endif
        
        HandleOption option(m_handle);
        auto res = option.handleNonBlock();
        if(!res) {
            ::close(m_handle.fd);
            m_handle.fd = -1;
        }
        return res;
    }


    std::expected<void, CommonError> AsyncUdpSocket::bind(const Host& addr)
    {
        using namespace error;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty()) addr_in.sin_addr.s_addr = INADDR_ANY;
        else addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::bind(m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
        {
            return std::unexpected(CommonError(CallBindError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> AsyncUdpSocket::connect(const Host& addr)
    {
        using namespace error;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty())
        {
            return std::unexpected(CommonError(CallConnectError, static_cast<uint32_t>(errno)));
        }
        addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::connect(m_handle.fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
        {
            return std::unexpected(CommonError(CallConnectError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::recv(char* result, size_t length)
    {
        return {std::make_shared<details::RecvEvent>(m_handle, m_scheduler, result, length)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::send(Bytes bytes)
    {
        return {std::make_shared<details::SendEvent>(m_handle, m_scheduler, std::move(bytes))};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::recvfrom(Host& remote, char* result, size_t length)
    {
        return {std::make_shared<details::RecvfromEvent>(m_handle, m_scheduler, remote, result, length)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::sendto(const Host& remote, Bytes bytes)
    {
        return {std::make_shared<details::SendtoEvent>(m_handle, m_scheduler, remote, std::move(bytes))};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncUdpSocket::close()
    {
        return {std::make_shared<details::CloseEvent>(m_handle, m_scheduler)};
    }

    std::expected<SockAddr, CommonError> AsyncUdpSocket::getSrcAddr() const
    {
        using namespace error;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        SockAddr saddr {};
        if (getsockname(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return std::unexpected(CommonError(CallGetSockNameError, static_cast<uint32_t>(errno)));
        }
        return sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
    }

    std::expected<SockAddr, CommonError> AsyncUdpSocket::getDestAddr() const
    {
        using namespace error;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        SockAddr saddr {};
        if (getpeername(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return std::unexpected(CommonError(CallGetPeerNameError, static_cast<uint32_t>(errno)));
        } 
        return sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
    }

    AsyncUdpSocket AsyncUdpSocket::cloneForDifferentRole(Runtime& runtime) const
    {
        return AsyncUdpSocket(runtime, m_handle);
    }

    AsyncSslSocket::AsyncSslSocket(Runtime& runtime)
    {
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    AsyncSslSocket::AsyncSslSocket(Runtime& runtime, SSL *ssl)
    {
        m_ssl = ssl;
        RuntimeVisitor visitor(runtime);
        m_scheduler = visitor.eventScheduler().get();
    }

    AsyncSslSocket::AsyncSslSocket(AsyncSslSocket &&other)
    {
        m_ssl = other.m_ssl;
        other.m_ssl = nullptr;
        m_scheduler = other.m_scheduler;
        other.m_scheduler = nullptr;
    }

    AsyncSslSocket &AsyncSslSocket::operator=(AsyncSslSocket &&other)
    {
        if(this != &other) {
            m_ssl = other.m_ssl;
            other.m_ssl = nullptr;
            m_scheduler = other.m_scheduler;
            other.m_scheduler = nullptr;
        }
        return *this;
    }

    AsyncSslSocket::AsyncSslSocket(EventScheduler* scheduler, SSL* ssl)
    {
        m_ssl = ssl;
        m_scheduler = scheduler;
    }

    HandleOption AsyncSslSocket::options()
    {
        return HandleOption(getHandle());
    }

    GHandle AsyncSslSocket::getHandle() const
    {
        return { SSL_get_fd(m_ssl) };
    }

    std::expected<void, CommonError> AsyncSslSocket::socket()
    {
        using namespace error;
        GHandle handle;
        handle.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (handle.fd < 0) {
            return std::unexpected(CommonError(CallSocketError, static_cast<uint32_t>(errno)));
        }
        
        // macOS上设置SO_NOSIGPIPE选项防止SIGPIPE信号
        #ifdef SO_NOSIGPIPE
        int set = 1;
        (void)setsockopt(handle.fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
        #endif
        
        HandleOption option(handle);
        auto res = option.handleNonBlock();
        if(!res) {
            ::close(handle.fd);
            handle.fd = -1;
            return res;
        }
        m_ssl = SSL_new(getGlobalSSLCtx());
        if(m_ssl == nullptr) {
            return std::unexpected(CommonError(CallSSLNewError, static_cast<uint32_t>(errno)));
        }
        SSL_set_fd(m_ssl, handle.fd);
        return {};
    }


    std::expected<void, CommonError> AsyncSslSocket::bind(const Host& addr)
    {
        using namespace error;
        sockaddr_in addr_in{};
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(addr.port);
        if(addr.ip.empty()) addr_in.sin_addr.s_addr = INADDR_ANY;
        else addr_in.sin_addr.s_addr = inet_addr(addr.ip.c_str());
        if(::bind(getHandle().fd, reinterpret_cast<sockaddr*>(&addr_in), sizeof(sockaddr)))
        {
            return std::unexpected(CommonError(CallBindError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> AsyncSslSocket::listen(int backlog)
    {
        using namespace error;
        if(::listen(getHandle().fd, backlog))
        {
            return std::unexpected(CommonError(CallListenError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncSslSocket::accept(AsyncSslSocketBuilder &builder)
    {
        builder.m_scheduler = m_scheduler;
        return {std::make_shared<details::AcceptEvent>(GHandle(SSL_get_fd(m_ssl)), m_scheduler, builder.m_handle)};
    }

    std::expected<void, CommonError> AsyncSslSocket::readyToSslAccept(AsyncSslSocketBuilder &builder)
    {
        builder.m_ssl = SSL_new(getGlobalSSLCtx());
        if(builder.m_ssl == nullptr) {
            close(builder.m_handle.fd);
            return std::unexpected(CommonError(CallSSLNewError, static_cast<uint32_t>(errno)));
        }
        if(SSL_set_fd(builder.m_ssl, builder.m_handle.fd) == -1) {
            SSL_free(builder.m_ssl);
            builder.m_ssl = nullptr;
            close(builder.m_handle.fd);
            return std::unexpected(CommonError(CallSSLSetFdError, static_cast<uint32_t>(errno)));
        }
        SSL_set_accept_state(builder.m_ssl);
        return {};
    }

    AsyncResult<std::expected<bool, CommonError>> AsyncSslSocket::sslAccept(AsyncSslSocketBuilder& builder)
    {
        return {std::make_shared<details::SslAcceptEvent>(builder.m_ssl, m_scheduler)};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncSslSocket::connect(const Host& addr)
    {
        return {std::make_shared<details::ConnectEvent>(GHandle(SSL_get_fd(m_ssl)), m_scheduler, addr)};
    }

    void AsyncSslSocket::readyToSslConnect()
    {
        SSL_set_connect_state(m_ssl);
    }

    AsyncResult<std::expected<bool, CommonError>> AsyncSslSocket::sslConnect()
    {
        return {std::make_shared<details::SslConnectEvent>(m_ssl, m_scheduler)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncSslSocket::sslRecv(char* result, size_t length)
    {
        return {std::make_shared<details::SslRecvEvent>(m_ssl, m_scheduler, result, length)};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncSslSocket::sslSend(Bytes bytes)
    {
        return {std::make_shared<details::SslSendEvent>(m_ssl, m_scheduler, std::move(bytes))};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncSslSocket::sslClose()
    {
        return {std::make_shared<details::SslCloseEvent>(m_ssl, m_scheduler)};
    }

    AsyncSslSocket AsyncSslSocket::cloneForDifferentRole(Runtime& runtime) const
    {
        return AsyncSslSocket(runtime, m_ssl);
    }

    SSL* AsyncSslSocket::getSsl() const
    {
        return m_ssl;
    }

    AsyncSslSocket::~AsyncSslSocket()
    {

    }

} // namespace galay