#include "UdpServer.h"


namespace galay
{
    UdpServer::UdpServer()
        : m_runtime(RuntimeBuilder().build())
    {
    }

    UdpServer::UdpServer(Runtime&& runtime)
        : m_runtime(std::move(runtime))
    {

    }

    UdpServer::UdpServer(UdpServer&& server)
        : m_runtime(std::move(server.m_runtime))
    {
        m_host = std::move(server.m_host);
    }

    void UdpServer::listenOn(Host host)
    {
        m_host = std::move(host);
    }

    void UdpServer::useStrategy(ServerStrategy strategy)
    {
        m_strategy = strategy;
    }

    void UdpServer::run(const std::function<Coroutine<nil>(AsyncUdpSocket,AsyncFactory)>& callback)
    {
        m_runtime.start();
        size_t co_num = m_runtime.coSchedulerSize();
        for(size_t i = 0; i < co_num; ++i) {
            AsyncUdpSocket socket(m_runtime);
            socket.socket();
            HandleOption options = socket.options();
            options.handleReuseAddr();
            options.handleReusePort();
            socket.bind(m_host);
            m_runtime.schedule(callback(std::move(socket),AsyncFactory(m_runtime)), i);
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
            m_host = std::move(server.m_host);
        }
        return *this;
    }

    UdpServer::~UdpServer()
    {
        m_runtime.stop();
    }

    UdpServerBuilder &UdpServerBuilder::addListen(const Host &host)
    {
        m_host = host;
        return *this;
    }

    UdpServerBuilder &UdpServerBuilder::startCoChecker(std::chrono::milliseconds interval)
    {
        m_coCheckerInterval = interval;
        return *this;
    }

    UdpServerBuilder &UdpServerBuilder::strategy(ServerStrategy strategy)
    {
        m_strategy = strategy;
        return *this;
    }

    UdpServerBuilder &UdpServerBuilder::timeout(int timeout)
    {
        m_timeout = timeout;
        return *this;
    }

    UdpServerBuilder &UdpServerBuilder::threads(int threads)
    {
        m_threads = threads;
        return *this;
    }

    UdpServer UdpServerBuilder::build()
    {
        RuntimeBuilder builder;
        builder.setCoSchedulerNum(m_threads);
        builder.setEventCheckTimeout(m_timeout);
        builder.startCoManager(m_coCheckerInterval);
        Runtime runtime = builder.build();
        UdpServer server(std::move(runtime));
        server.listenOn(m_host);
        server.useStrategy(m_strategy);
        return server;
    }

}