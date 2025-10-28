#include "TcpServer.h"
#include "galay/kernel/async/AsyncFactory.h"

namespace galay
{
    TcpServer::TcpServer(TcpServer &&server)
        : m_sockets(std::move(server.m_sockets))
    {
        m_host = std::move(server.m_host);
        m_backlog = server.m_backlog;
    }

    void TcpServer::listenOn(Host host, int backlog)
    {
        m_host = std::move(host);
        m_backlog = backlog;
    }

    bool TcpServer::run(Runtime& runtime, const AsyncTcpFunc &callback)
    {
        size_t co_num = runtime.coSchedulerSize();
        AsyncFactory factory = runtime.getAsyncFactory();
        for(size_t i = 0; i < co_num; ++i) {
            m_sockets.emplace_back(factory.getTcpSocket());
            if(auto res = m_sockets[i].socket(); !res) {
                LogError("[TcpServer::run] [error: {}]", res.error().message());
                return false;
            }
            HandleOption options = m_sockets[i].options();
            if(auto res = options.handleReuseAddr(); !res) {
                LogError("[TcpServer::run] [error: {}]", res.error().message());
                return false;
            }
            if(auto res = options.handleReusePort(); !res) {
                LogError("[TcpServer::run] [error: {}]", res.error().message());
                return false;
            }
            if(auto res = m_sockets[i].bind(m_host); !res) {
                LogError("[TcpServer::run] [error: {}]", res.error().message());
                return false;
            }
            if(auto res = m_sockets[i].listen(m_backlog); !res) {
                LogError("[TcpServer::run] [error: {}]", res.error().message());
                return false;
            }
            runtime.schedule(acceptConnection(runtime, callback, i), i);
        }
        return true;
    }

    void TcpServer::stop()
    {
        m_condition.notify_one();
    }

    void TcpServer::wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock);
    }

    TcpServer &TcpServer::operator=(TcpServer &&server)
    {
        if(this != &server) {
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
            m_backlog = server.m_backlog;
        }
        return *this;
    }

    Coroutine<nil> TcpServer::acceptConnection(Runtime& runtime, AsyncTcpFunc callback, size_t i)
    {
        while(true) {
            AsyncTcpSocketBuilder builder;
            auto acceptor = co_await m_sockets[i].accept(builder);
            if(acceptor) {
                LogInfo("[acceptConnection success]");
                auto socket = builder.build();
                runtime.schedule(callback(std::move(socket)), i);
            } else {
                LogError("[acceptConnection failed] [error: {}]", acceptor.error().message());
            }
        }
        co_return nil();
    }

    TcpServerBuilder &TcpServerBuilder::addListen(const Host &host)
    {
        m_host = host;
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::backlog(int backlog)
    {
        m_backlog = backlog;
        return *this;
    }
    
    TcpServer TcpServerBuilder::build()
    {
        TcpServer server;
        server.listenOn(m_host, m_backlog);
        return server;
    }
}