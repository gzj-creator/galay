#include "TcpSslServer.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/runtime/Runtime.h"
#include <sys/socket.h>
#include <unistd.h>

namespace galay
{
    TcpSslServer::TcpSslServer(const std::string& cert_file, const std::string& key_file)
        : m_cert(cert_file), m_key(key_file)
    {
    }

    TcpSslServer::TcpSslServer(TcpSslServer&& server)
        : m_sockets(std::move(server.m_sockets)),
          m_running(server.m_running.load())
    {
        m_host = std::move(server.m_host);
        m_backlog = server.m_backlog;
        m_cert = std::move(server.m_cert);
        m_key = std::move(server.m_key);
        m_ssl_ctx = server.m_ssl_ctx;
        server.m_ssl_ctx = nullptr;
        server.m_running.store(false);
    }
    
    bool TcpSslServer::initSSLContext()
    {
        if(m_ssl_ctx) {
            return true;  // Already initialized
        }
        
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        
        m_ssl_ctx = SSL_CTX_new(TLS_server_method());
        if(!m_ssl_ctx) {
            return false;
        }
        
        if(SSL_CTX_use_certificate_file(m_ssl_ctx, m_cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
            SSL_CTX_free(m_ssl_ctx);
            m_ssl_ctx = nullptr;
            return false;
        }
        
        if(SSL_CTX_use_PrivateKey_file(m_ssl_ctx, m_key.c_str(), SSL_FILETYPE_PEM) <= 0) {
            SSL_CTX_free(m_ssl_ctx);
            m_ssl_ctx = nullptr;
            return false;
        }
        
        return true;
    }

    void TcpSslServer::listenOn(Host host, int backlog)
    {
        m_host = std::move(host);
        m_backlog = backlog;
    }
    
    bool TcpSslServer::initializeSSLContext()
    {
        return initSSLContext();
    }
    
    SSL_CTX* TcpSslServer::getSSLContext() const
    {
        return m_ssl_ctx;
    }

    void TcpSslServer::run(Runtime& runtime, const AsyncSslFunc &callback)
    {
        if(m_cert.empty() || m_key.empty()) {
            throw std::runtime_error("cert or key is empty");
        }
        
        if(!initSSLContext()) {
            throw std::runtime_error("Failed to initialize SSL context");
        }
        
        m_running.store(true);
        size_t co_num = runtime.coSchedulerSize();
        for(size_t i = 0; i < co_num; ++i) {
            auto handle = runtime.getCoSchedulerHandle(i);
            if(handle.has_value()) {
                m_sockets.emplace_back(handle.value().getAsyncFactory().getSslSocket(m_ssl_ctx));
                if(auto res = m_sockets[i].socket(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                HandleOption options = m_sockets[i].options();
                if(auto res = options.handleReuseAddr(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = options.handleReusePort(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = m_sockets[i].bind(m_host); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = m_sockets[i].listen(m_backlog); !res) {
                    throw std::runtime_error(res.error().message());
                }
                handle.value().spawn(acceptConnection(handle.value(), callback, i));
            } else {
                throw std::runtime_error("[SslServer::run] [invalid handle index]");
            }
        }
    }

    void TcpSslServer::stop()
    {
        m_running.store(false);
        // 关闭所有监听 socket，让挂起的 accept 立即返回错误
        for(auto& sock : m_sockets) {
            int fd = SSL_get_fd(sock.getSsl());
            if(fd >= 0) {
                // 关闭 socket 会导致挂起的 accept 返回错误
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        m_condition.notify_one();
    }

    void TcpSslServer::wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock);
    }

    TcpSslServer &TcpSslServer::operator=(TcpSslServer &&server)
    {
        if(this != &server) {
            if(m_ssl_ctx) {
                SSL_CTX_free(m_ssl_ctx);
            }
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
            m_backlog = server.m_backlog;
            m_cert = std::move(server.m_cert);
            m_key = std::move(server.m_key);
            m_ssl_ctx = server.m_ssl_ctx;
            server.m_ssl_ctx = nullptr;
        }
        return *this;
    }

    Coroutine<nil> TcpSslServer::acceptConnection(CoSchedulerHandle handle, AsyncSslFunc callback, size_t i)
    {
        while(m_running.load()) {
            AsyncSslSocketBuilder builder(m_ssl_ctx);
            auto acceptor = co_await m_sockets[i].accept(builder);
            // 立即检查运行状态，防止在 stop 后继续执行
            if(!m_running.load()) {
                LogInfo("[acceptConnection stopped after accept] [thread: {}]", i);
                break;
            }
            if(!acceptor) {
                LogError("[acceptConnection failed] [error: {}]", acceptor.error().message());
                continue;
            } 
            if( auto res = m_sockets[i].readyToSslAccept(builder); !res) {
                LogError("[state mod failed] [error: {}]", res.error().message());
                continue;
            }
            std::expected<bool, CommonError> res;
            while (m_running.load())
            {
                res = co_await m_sockets[i].sslAccept(builder);
                // 每次 co_await 返回后都检查
                if(!m_running.load()) {
                    LogInfo("[acceptConnection stopped during sslAccept] [thread: {}]", i);
                    co_return nil();
                }
                if(!res) {
                    LogError("[sslAccept failed] [error: {}]", res.error().message());
                    break;
                } 
                if(res.value()) {
                    break;
                }
            }
            if(!res || !res.value()) continue;
            LogInfo("[sslAccept success]");
            auto socket = builder.build();
            handle.spawn(callback(std::move(socket), handle));
        }
        LogInfo("[acceptConnection loop exit] [thread: {}]", i);
        co_return nil();
    }

    TcpSslServer::~TcpSslServer()
    {
        if(m_ssl_ctx) {
            SSL_CTX_free(m_ssl_ctx);
            m_ssl_ctx = nullptr;
        }
    }


    TcpSslServerBuilder::TcpSslServerBuilder(const std::string &cert, const std::string &key)
    {
        m_cert = cert;
        m_key = key;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::addListen(const Host &host)
    {
        m_host = host;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::backlog(int backlog)
    {
        m_backlog = backlog;
        return *this;
    }

    TcpSslServer TcpSslServerBuilder::build()
    {
        TcpSslServer server(m_cert, m_key);
        server.listenOn(m_host, m_backlog);
        return server;
    }
}