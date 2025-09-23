#include "TcpSslServer.h"

namespace galay
{
    TcpSslServer::TcpSslServer(const std::string& cert_file, const std::string& key_file)
        : m_runtime(RuntimeBuilder().build())
    {
        
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncSslSocket(m_runtime));
        }
    }

    TcpSslServer::TcpSslServer(Runtime&& runtime, const std::string& cert_file, const std::string& key_file)
        : m_runtime(std::move(runtime)), m_cert(cert_file), m_key(key_file)
    {
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncSslSocket(m_runtime));
        }
    }

    TcpSslServer::TcpSslServer(TcpSslServer&& server)
        : m_runtime(std::move(server.m_runtime)),
            m_sockets(std::move(server.m_sockets))
    {
        m_host = std::move(server.m_host);
        m_backlog = server.m_backlog;
        m_cert = std::move(server.m_cert);
        m_key = std::move(server.m_key);
    }

    void TcpSslServer::listenOn(Host host, int backlog)
    {
        m_host = std::move(host);
        m_backlog = backlog;
    }

    void TcpSslServer::useStrategy(ServerStrategy strategy)
    {
        m_strategy = strategy;
    }

    void TcpSslServer::run(const std::function<Coroutine<nil>(AsyncSslSocket, AsyncFactory)> &callback)
    {
        if(m_cert.empty() || m_key.empty()) {
            throw std::runtime_error("cert or key is empty");
        } else {
            if(getGlobalSSLCtx() == nullptr) {
                initializeSSLServerEnv(m_cert.c_str(), m_key.c_str());
            }
        }
        m_runtime.start();
        size_t co_num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < co_num; ++i) {
            m_sockets[i].socket();
            HandleOption options = m_sockets[i].options();
            options.handleReuseAddr();
            options.handleReusePort();
            m_sockets[i].bind(m_host);
            m_sockets[i].listen(m_backlog);
            m_runtime.schedule(acceptConnection(callback, i), i);
        }
    }

    void TcpSslServer::stop()
    {
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
            m_runtime = std::move(server.m_runtime);
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
            m_backlog = server.m_backlog;
            m_cert = std::move(server.m_cert);
            m_key = std::move(server.m_key);
        }
        return *this;
    }

    Coroutine<nil> TcpSslServer::acceptConnection(const std::function<Coroutine<nil>(AsyncSslSocket,AsyncFactory)>& callback, size_t i)
    {
        while(true) {
            AsyncSslSocketBuilder builder;
            if(auto acceptor = co_await m_sockets[i].accept(builder); !acceptor) {
                LogError("[acceptConnection failed] [error: {}]", acceptor.error().message());
                continue;
            } 
            if( auto res = m_sockets[i].readyToSslAccept(builder); !res) {
                LogError("[state mod failed] [error: {}]", res.error().message());
                continue;
            }
            std::expected<bool, CommonError> res;
            while (true)
            {
                res = co_await m_sockets[i].sslAccept(builder);
                if(!res) {
                    LogError("[sslAccept failed] [error: {}]", res.error().message());
                    break;
                } 
                if(res.value()) {
                    LogInfo("[sslAccept success]");
                    break;
                }
            }
            if(!res) continue;
            auto socket = builder.build();
            m_runtime.schedule(callback(std::move(socket),AsyncFactory(m_runtime)), i);
        }
        co_return nil();
    }

    TcpSslServer::~TcpSslServer()
    {
        m_runtime.stop();
        if(getGlobalSSLCtx() != nullptr) { 
            destroySSLEnv();
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

    TcpSslServerBuilder &TcpSslServerBuilder::startCoChecker(std::chrono::milliseconds interval)
    {
        m_coCheckerInterval = interval;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::strategy(ServerStrategy strategy)
    {
        m_strategy = strategy;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::timeout(int timeout)
    {
        m_timeout = timeout;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }
    
    TcpSslServer TcpSslServerBuilder::build()
    {
        RuntimeBuilder builder;
        builder.setCoSchedulerNum(m_threads);
        builder.setEventCheckTimeout(m_timeout);
        builder.startCoManager(m_coCheckerInterval);
        Runtime runtime = builder.build();
        TcpSslServer server(std::move(runtime), m_cert, m_key);
        server.listenOn(m_host, m_backlog);
        server.useStrategy(m_strategy);
        return server;
    }
}