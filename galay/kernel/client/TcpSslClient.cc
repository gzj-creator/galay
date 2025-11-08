#include "TcpSslClient.h"
#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include <cassert>

namespace galay
{
    bool TcpSslClient::initSSLContext(const char* server_pem)
    {
        if(m_ssl_ctx) {
            return true;
        }
        
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        
        m_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if(!m_ssl_ctx) {
            return false;
        }
        
        if(server_pem) {
            if(SSL_CTX_load_verify_locations(m_ssl_ctx, server_pem, NULL) <= 0) {
                SSL_CTX_free(m_ssl_ctx);
                m_ssl_ctx = nullptr;
                return false;
            }
        }
        
        m_owns_ctx = true;
        return true;
    }

    TcpSslClient::TcpSslClient(CoSchedulerHandle handle, const char* server_pem)
        :m_socket(handle, static_cast<SSL_CTX*>(nullptr))
    {
        if(!initSSLContext(server_pem)) {
            throw std::runtime_error("Failed to initialize SSL context");
        }
        m_socket = AsyncSslSocket(handle, m_ssl_ctx);
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
    }

    TcpSslClient::TcpSslClient(CoSchedulerHandle handle, SSL_CTX* ssl_ctx)
        :m_socket(handle, ssl_ctx), m_ssl_ctx(ssl_ctx), m_owns_ctx(false)
    {
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
    }

    TcpSslClient::TcpSslClient(AsyncSslSocket &&socket)
        :m_socket(std::move(socket)), m_ssl_ctx(nullptr), m_owns_ctx(false)
    {
    }

    TcpSslClient::TcpSslClient(CoSchedulerHandle handle, const Host &bind_addr, const char* server_pem) 
        :m_socket(handle, static_cast<SSL_CTX*>(nullptr))
    {
        if(!initSSLContext(server_pem)) {
            throw std::runtime_error("Failed to initialize SSL context");
        }
        m_socket = AsyncSslSocket(handle, m_ssl_ctx);
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
        std::expected<void, CommonError> bind_res = m_socket.bind(bind_addr);
        if(!bind_res) {
            throw std::runtime_error(bind_res.error().message());
        }
    }

    TcpSslClient::TcpSslClient(CoSchedulerHandle handle, const Host &bind_addr, SSL_CTX* ssl_ctx)
        :m_socket(handle, ssl_ctx), m_ssl_ctx(ssl_ctx), m_owns_ctx(false)
    {
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
        std::expected<void, CommonError> bind_res = m_socket.bind(bind_addr);
        if(!bind_res) {
            throw std::runtime_error(bind_res.error().message());
        }
    }

    TcpSslClient::~TcpSslClient()
    {
        if(m_owns_ctx && m_ssl_ctx) {
            SSL_CTX_free(m_ssl_ctx);
            m_ssl_ctx = nullptr;
        }
    }

    AsyncResult<std::expected<void, CommonError>> TcpSslClient::connect(const Host& addr)
    {
        return m_socket.connect(addr);
    }

    void TcpSslClient::readyToSslConnect()
    {
        m_socket.readyToSslConnect();
    }

    AsyncResult<std::expected<bool, CommonError>> TcpSslClient::sslConnect()
    {
        return m_socket.sslConnect();
    }

    AsyncResult<std::expected<Bytes, CommonError>> TcpSslClient::sslRecv(char* buffer, size_t length)
    {
        return m_socket.sslRecv(buffer, length);
    }

    AsyncResult<std::expected<Bytes, CommonError>> TcpSslClient::sslSend(Bytes bytes)
    {
        return m_socket.sslSend(std::move(bytes));
    }

    AsyncResult<std::expected<void, CommonError>> TcpSslClient::close()
    {
        return m_socket.sslClose();
    }

    TcpSslClient TcpSslClient::cloneForDifferentRole(CoSchedulerHandle handle) const
    {
        return TcpSslClient(m_socket.cloneForDifferentRole(handle));
    }
}
