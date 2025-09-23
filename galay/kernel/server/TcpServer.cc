#include "TcpServer.h"
#include "galay/utils/System.h"

namespace galay
{
    TcpServer::TcpServer()
        : m_runtime(RuntimeBuilder().build())
    {
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncTcpSocket(m_runtime));
        }
    }

    TcpServer::TcpServer(Runtime &&runtime)
        : m_runtime(std::move(runtime))
    {
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncTcpSocket(m_runtime));
        }
    }

    TcpServer::TcpServer(TcpServer &&server)
        : m_runtime(std::move(server.m_runtime)),
            m_sockets(std::move(server.m_sockets))
    {
        m_host = std::move(server.m_host);
        m_backlog = server.m_backlog;
        m_strategy = server.m_strategy;
    }

    void TcpServer::listenOn(Host host, int backlog)
    {
        m_host = std::move(host);
        m_backlog = backlog;
    }

    void TcpServer::useStrategy(ServerStrategy strategy)
    {
        m_strategy = strategy;
    }

    bool TcpServer::run(const std::function<Coroutine<nil>(AsyncTcpSocket, AsyncFactory)> &callback)
    {
        switch (m_strategy)
        {
        case ServerStrategy::SingleRuntime:
        {
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
            return true;
        }
        default:
            break;
        }
        return false;
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
            m_runtime = std::move(server.m_runtime);
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
            m_backlog = server.m_backlog;
        }
        return *this;
    }

    TcpServer::~TcpServer()
    {
        m_runtime.stop();
    }

    Coroutine<nil> TcpServer::acceptConnection(const std::function<Coroutine<nil>(AsyncTcpSocket,AsyncFactory)>& callback, size_t i)
    {
        while(true) {
            AsyncTcpSocketBuilder builder;
            auto acceptor = co_await m_sockets[i].accept(builder);
            if(acceptor) {
                LogInfo("[acceptConnection success]");
                auto socket = builder.build();
                m_runtime.schedule(callback(std::move(socket), AsyncFactory(m_runtime)), i);
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

    TcpServerBuilder &TcpServerBuilder::startCoChecker(std::chrono::milliseconds interval)
    {
        m_coCheckerInterval = interval;
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::strategy(ServerStrategy strategy)
    {
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::timeout(int timeout)
    {
        m_timeout = timeout;
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }

    TcpServer TcpServerBuilder::build()
    {
        RuntimeBuilder builder;
        builder.setCoSchedulerNum(m_threads);
        builder.setEventCheckTimeout(m_timeout);
        builder.startCoManager(m_coCheckerInterval);
        Runtime runtime = builder.build();
        TcpServer server(std::move(runtime));
        server.listenOn(m_host, m_backlog);
        server.useStrategy(m_strategy);
        return server;
    }
}