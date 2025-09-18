#include "TcpServer.h"

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
    }


    void TcpServer::run(const std::function<Coroutine<nil>(AsyncTcpSocket,AsyncFactory)>& callback)
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
            auto acceptor = co_await m_sockets[i].accept();
            if(acceptor) {
                LogInfo("[acceptConnection success]");
                auto& builder = acceptor.value();
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
        m_server.m_host = host;
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::backlog(int backlog)
    {
        m_server.m_backlog = backlog;
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::startCoChecker(bool start, std::chrono::milliseconds interval)
    {
        if(start) {
            m_server.m_runtime.startCoManager(interval);
        } else {
            m_server.m_runtime.startCoManager(std::chrono::milliseconds(-1));
        }
        return *this;
    }

    TcpServer TcpServerBuilder::build()
    {
        TcpServer server = std::move(m_server);
        m_server = TcpServer{};
        return server;
    }
}