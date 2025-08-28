#include "UdpServer.h"


namespace galay
{
    UdpServer::UdpServer()
        : m_runtime(RuntimeBuilder().build())
    {
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncUdpSocket(m_runtime));
        }
    }

    UdpServer::UdpServer(Runtime&& runtime)
        : m_runtime(std::move(runtime))
    {
        size_t num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < num; ++i) {
            m_sockets.emplace_back(AsyncUdpSocket(m_runtime));
        }
    }

    UdpServer::UdpServer(UdpServer&& server)
        : m_runtime(std::move(server.m_runtime)),
            m_sockets(std::move(server.m_sockets))
    {
        m_host = std::move(server.m_host);
    }

    void UdpServer::run(const std::function<Coroutine<nil>(AsyncUdpSocket&)>& callback)
    {
        m_runtime.start();
        size_t co_num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < co_num; ++i) {
            m_sockets[i].socket();
            HandleOption options = m_sockets[i].options();
            options.handleNonBlock();
            options.handleReuseAddr();
            options.handleReusePort();
            m_sockets[i].bind(m_host);
            m_runtime.schedule(dealConnection(callback, i), i);
        }
    }

    void UdpServer::stop()
    {
        m_condition.notify_one();
    }

    void UdpServer::wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock);
    }

    UdpServer &UdpServer::operator=(UdpServer &&server)
    {
        if(this != &server) {
            m_runtime = std::move(server.m_runtime);
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
        }
        return *this;
    }

    Coroutine<nil> UdpServer::dealConnection(const std::function<Coroutine<nil>(AsyncUdpSocket&)>& callback, size_t i)
    {
        AsyncUdpSocket socket = m_sockets[i];
        m_runtime.schedule(callback(socket), i);
        co_return nil();
    }

    UdpServer::~UdpServer()
    {
        m_runtime.stop();
    }

    UdpServerBuilder &UdpServerBuilder::addListen(const Host &host)
    {
        m_server.m_host = host;
        return *this;
    }

    UdpServerBuilder &UdpServerBuilder::startCoChecker(bool start, std::chrono::milliseconds interval)
    {
        if(start) {
            m_server.m_runtime.startCoManager(interval);
        } else {
            m_server.m_runtime.startCoManager(std::chrono::milliseconds(-1));
        }
        return *this;
    }

    UdpServer UdpServerBuilder::build()
    {
        UdpServer server = std::move(m_server);
        m_server = UdpServer{};
        return server;
    }
}