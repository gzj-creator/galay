#include "Socket.h"

#include "galay/common/Error.h"
#include <cassert>
#include <memory>
#include <openssl/err.h>

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

    AsyncTcpSocket::AsyncTcpSocket(CoSchedulerHandle handle)
    {
        m_scheduler = handle.eventScheduler();
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
    }

    AsyncTcpSocket::AsyncTcpSocket(CoSchedulerHandle handle, GHandle fd)
    {
        m_handle = fd;
        m_scheduler = handle.eventScheduler();
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        HandleOption options(fd);
        if (auto res = options.handleNonBlock(); !res) {
            LogError("handleNonBlock failed");
            m_handle = GHandle::invalid();
        }
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        return HandleOption(m_handle);
    }

    std::expected<void, CommonError> AsyncTcpSocket::socket()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        using namespace error;
        if(::listen(m_handle.fd, backlog))
        {
            return std::unexpected(CommonError(CallListenError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> AsyncTcpSocket::shuntdown(ShutdownType type)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_closeEvent.reset(m_handle, m_scheduler);
        return {std::shared_ptr<details::CloseEvent>(&m_closeEvent, [](details::CloseEvent*){})};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncTcpSocket::accept(AsyncTcpSocketBuilder& builder)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        builder.m_scheduler = m_scheduler;
        m_acceptEvent.reset(m_handle, m_scheduler, &builder.m_handle);
        return {std::shared_ptr<details::AcceptEvent>(&m_acceptEvent, [](details::AcceptEvent*){})};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncTcpSocket::connect(const Host& host)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_connectEvent.reset(m_handle, m_scheduler, host);
        return {std::shared_ptr<details::ConnectEvent>(&m_connectEvent, [](details::ConnectEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncTcpSocket::recv(char* result, size_t length)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_recvEvent.reset(m_handle, m_scheduler, result, length);
        return {std::shared_ptr<details::RecvEvent>(&m_recvEvent, [](details::RecvEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncTcpSocket::send(Bytes bytes)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_sendEvent.reset(m_handle, m_scheduler, std::move(bytes));
        return {std::shared_ptr<details::SendEvent>(&m_sendEvent, [](details::SendEvent*){})};
    }

#ifdef __linux__
    AsyncResult<std::expected<long, CommonError>> AsyncTcpSocket::sendfile(GHandle file_handle, long offset, size_t length)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_sendfileEvent.reset(m_handle, m_scheduler, file_handle, offset, length);
        return {std::shared_ptr<details::SendfileEvent>(&m_sendfileEvent, [](details::SendfileEvent*){})};
    }
#endif

    std::expected<SockAddr, CommonError> AsyncTcpSocket::getSrcAddr() const
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        using namespace error;
        std::expected<SockAddr, CommonError> wrapper;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);

        if (getpeername(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return std::unexpected(CommonError(CallGetPeerNameError, static_cast<uint32_t>(errno)));
        }
        return sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
    }

    AsyncTcpSocket AsyncTcpSocket::clone() const
    {
        return AsyncTcpSocket(m_scheduler, m_handle);
    }

    AsyncTcpSocket AsyncTcpSocket::clone(CoSchedulerHandle handle) const
    {
        return AsyncTcpSocket(handle, m_handle);
    }

    AsyncUdpSocket::AsyncUdpSocket(CoSchedulerHandle handle)
    {
        m_scheduler = handle.eventScheduler();
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
    }

    AsyncUdpSocket::AsyncUdpSocket(CoSchedulerHandle handle, GHandle fd)
    {
        m_handle = fd;
        m_scheduler = handle.eventScheduler();
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        return HandleOption(m_handle);
    }

    std::expected<void, CommonError> AsyncUdpSocket::socket()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_recvEvent.reset(m_handle, m_scheduler, result, length);
        return {std::shared_ptr<details::RecvEvent>(&m_recvEvent, [](details::RecvEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::send(Bytes bytes)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_sendEvent.reset(m_handle, m_scheduler, std::move(bytes));
        return {std::shared_ptr<details::SendEvent>(&m_sendEvent, [](details::SendEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::recvfrom(Host& remote, char* result, size_t length)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_recvfromEvent.reset(m_handle, m_scheduler, &remote, result, length);
        return {std::shared_ptr<details::RecvfromEvent>(&m_recvfromEvent, [](details::RecvfromEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncUdpSocket::sendto(const Host& remote, Bytes bytes)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_sendtoEvent.reset(m_handle, m_scheduler, remote, std::move(bytes));
        return {std::shared_ptr<details::SendtoEvent>(&m_sendtoEvent, [](details::SendtoEvent*){})};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncUdpSocket::close()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_closeEvent.reset(m_handle, m_scheduler);
        return {std::shared_ptr<details::CloseEvent>(&m_closeEvent, [](details::CloseEvent*){})};
    }

    std::expected<SockAddr, CommonError> AsyncUdpSocket::getSrcAddr() const
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        using namespace error;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        SockAddr saddr {};
        if (getpeername(m_handle.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return std::unexpected(CommonError(CallGetPeerNameError, static_cast<uint32_t>(errno)));
        } 
        return sockAddrToTHost(reinterpret_cast<sockaddr*>(&addr));
    }

    AsyncUdpSocket AsyncUdpSocket::clone() const
    {
        return AsyncUdpSocket(m_scheduler, m_handle);
    }

    AsyncUdpSocket AsyncUdpSocket::clone(CoSchedulerHandle handle) const
    {
        return AsyncUdpSocket(handle, m_handle);
    }

    AsyncUdpSocket::AsyncUdpSocket(EventScheduler* scheduler, GHandle handle)
    {
        m_handle = handle;
        m_scheduler = scheduler;
    }
    
    AsyncSslSocket::AsyncSslSocket(CoSchedulerHandle handle, SSL_CTX* ssl_ctx)
    {
        m_scheduler = handle.eventScheduler();
        LogTrace("[AsyncSslSocket constructor] m_scheduler: {}", static_cast<void*>(m_scheduler));
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        if(ssl_ctx) {
            m_ssl = SSL_new(ssl_ctx);
            if(m_ssl == nullptr) {
                unsigned long err = ERR_get_error();
                char err_buf[256];
                ERR_error_string_n(err, err_buf, sizeof(err_buf));
                throw std::runtime_error(err_buf);
            }
        }
    }

    AsyncSslSocket::AsyncSslSocket(CoSchedulerHandle handle, SSL *ssl)
    {
        m_ssl = ssl;
        m_scheduler = handle.eventScheduler();
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
    }

    AsyncSslSocket::AsyncSslSocket(AsyncSslSocket &&other)
    {
        LogTrace("[AsyncSslSocket move constructor] source m_scheduler: {}, target m_scheduler before: {}", 
                 static_cast<void*>(other.m_scheduler), static_cast<void*>(m_scheduler));
        m_ssl = other.m_ssl;
        other.m_ssl = nullptr;
        m_scheduler = other.m_scheduler;
        // 注意：移动后，新对象（this）应该有正确的 m_scheduler
        LogTrace("[AsyncSslSocket move constructor] target m_scheduler after: {}", static_cast<void*>(m_scheduler));
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        return HandleOption(getHandle());
    }

    GHandle AsyncSslSocket::getHandle() const
    {
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        return { SSL_get_fd(m_ssl) };
    }

    std::expected<void, CommonError> AsyncSslSocket::socket()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
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
        if(m_ssl) {
            SSL_set_fd(m_ssl, handle.fd);
        }
        return {};
    }


    std::expected<void, CommonError> AsyncSslSocket::bind(const Host& addr)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
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
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        using namespace error;
        if(::listen(getHandle().fd, backlog))
        {
            return std::unexpected(CommonError(CallListenError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncSslSocket::accept(AsyncSslSocketBuilder &builder)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        LogTrace("[AsyncSslSocket::accept] this m_scheduler: {}", static_cast<void*>(m_scheduler));
        builder.m_scheduler = m_scheduler;
        m_acceptEvent.reset(GHandle(SSL_get_fd(m_ssl)), m_scheduler, &builder.m_handle);
        return {std::shared_ptr<details::AcceptEvent>(&m_acceptEvent, [](details::AcceptEvent*){})};
    }

    std::expected<void, CommonError> AsyncSslSocket::readyToSslAccept(AsyncSslSocketBuilder &builder)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        if(!builder.m_ssl_ctx) {
            LogError("[readyToSslAccept] SSL_CTX is nullptr!");
            close(builder.m_handle.fd);
            return std::unexpected(CommonError(GlobalSSLCtxNotInitializedError, static_cast<uint32_t>(errno)));
        }
        LogTrace("[readyToSslAccept] Creating SSL object from SSL_CTX: {}", static_cast<void*>(builder.m_ssl_ctx));
        builder.m_ssl = SSL_new(builder.m_ssl_ctx);
        if(builder.m_ssl == nullptr) {
            unsigned long err = ERR_get_error();
            char err_buf[256];
            ERR_error_string_n(err, err_buf, sizeof(err_buf));
            LogError("[readyToSslAccept] SSL_new failed: {}", err_buf);
            close(builder.m_handle.fd);
            return std::unexpected(CommonError(CallSSLNewError, static_cast<uint32_t>(errno)));
        }
        LogTrace("[readyToSslAccept] SSL object created: {}, setting fd: {}", static_cast<void*>(builder.m_ssl), builder.m_handle.fd);
        if(SSL_set_fd(builder.m_ssl, builder.m_handle.fd) == -1) {
            unsigned long err = ERR_get_error();
            char err_buf[256];
            ERR_error_string_n(err, err_buf, sizeof(err_buf));
            LogError("[readyToSslAccept] SSL_set_fd failed: {}", err_buf);
            SSL_free(builder.m_ssl);
            builder.m_ssl = nullptr;
            close(builder.m_handle.fd);
            return std::unexpected(CommonError(CallSSLSetFdError, static_cast<uint32_t>(errno)));
        }
        SSL_set_accept_state(builder.m_ssl);
        LogTrace("[readyToSslAccept] SSL object ready for accept");
        return {};
    }

    AsyncResult<std::expected<bool, CommonError>> AsyncSslSocket::sslAccept(AsyncSslSocketBuilder& builder)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        m_sslAcceptEvent.reset(builder.m_ssl, m_scheduler);
        return {std::shared_ptr<details::SslAcceptEvent>(&m_sslAcceptEvent, [](details::SslAcceptEvent*){})};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncSslSocket::connect(const Host& addr)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        m_connectEvent.reset(GHandle(SSL_get_fd(m_ssl)), m_scheduler, addr);
        return {std::shared_ptr<details::ConnectEvent>(&m_connectEvent, [](details::ConnectEvent*){})};
    }

    void AsyncSslSocket::readyToSslConnect()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        SSL_set_connect_state(m_ssl);
    }

    AsyncResult<std::expected<bool, CommonError>> AsyncSslSocket::sslConnect()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        m_sslConnectEvent.reset(m_ssl, m_scheduler);
        return {std::shared_ptr<details::SslConnectEvent>(&m_sslConnectEvent, [](details::SslConnectEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncSslSocket::sslRecv(char* result, size_t length)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        m_sslRecvEvent.reset(m_ssl, m_scheduler, result, length);
        return {std::shared_ptr<details::SslRecvEvent>(&m_sslRecvEvent, [](details::SslRecvEvent*){})};
    }

    AsyncResult<std::expected<Bytes, CommonError>> AsyncSslSocket::sslSend(Bytes bytes)
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        m_sslSendEvent.reset(m_ssl, m_scheduler, std::move(bytes));
        return {std::shared_ptr<details::SslSendEvent>(&m_sslSendEvent, [](details::SslSendEvent*){})};
    }

    AsyncResult<std::expected<void, CommonError>> AsyncSslSocket::sslClose()
    {
        assert(m_scheduler != nullptr && "EventScheduler cannot be nullptr");
        assert(m_ssl != nullptr && "SSL cannot be nullptr");
        m_sslCloseEvent.reset(m_ssl, m_scheduler);
        return {std::shared_ptr<details::SslCloseEvent>(&m_sslCloseEvent, [](details::SslCloseEvent*){})};
    }

    AsyncSslSocket AsyncSslSocket::clone() const    
    {
        return AsyncSslSocket(m_scheduler, m_ssl);
    }

    AsyncSslSocket AsyncSslSocket::clone(CoSchedulerHandle handle) const
    {
        return AsyncSslSocket(handle, m_ssl);
    }

    SSL* AsyncSslSocket::getSsl() const
    {
        return m_ssl;
    }

    AsyncSslSocket::~AsyncSslSocket()
    {

    }

} // namespace galay