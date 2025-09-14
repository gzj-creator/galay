#include "TcpSslServer.h"

namespace galay
{
    TcpSslServer::TcpSslServer()
        : m_runtime(RuntimeBuilder().build())
    {
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncSslSocket(m_runtime));
        }
    }

    TcpSslServer::TcpSslServer(Runtime&& runtime)
        : m_runtime(std::move(runtime))
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

    void TcpSslServer::run(const std::function<Coroutine<nil>(AsyncSslSocket,AsyncFactory)>& callback)
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
            options.handleNonBlock();
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
            auto wrapper = co_await m_sockets[i].sslAccept();
            if(wrapper.success()) {
                LogInfo("[acceptConnection success]");
                auto builder = wrapper.moveValue();
                auto socket = builder.build();
                socket.options().handleNonBlock();
                m_runtime.schedule(callback(std::move(socket),AsyncFactory(m_runtime, i)), i);
            } else {
                LogError("[acceptConnection failed] [error: {}]", wrapper.getError()->message());
            }
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

    TcpSslServerBuilder& TcpSslServerBuilder::sslConf(const std::string& cert, const std::string& key)
    {
        m_server.m_cert = cert;
        m_server.m_key = key;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::addListen(const Host &host)
    {
        m_server.m_host = host;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::backlog(int backlog)
    {
        m_server.m_backlog = backlog;
        return *this;
    }

    TcpSslServerBuilder &TcpSslServerBuilder::startCoChecker(bool start, std::chrono::milliseconds interval)
    {
        if(start) {
            m_server.m_runtime.startCoManager(interval);
        } else {
            m_server.m_runtime.startCoManager(std::chrono::milliseconds(-1));
        }
        return *this;
    }

    TcpSslServer TcpSslServerBuilder::build()
    {
        TcpSslServer server = std::move(m_server);
        m_server = TcpSslServer{};
        return server;
    }
}