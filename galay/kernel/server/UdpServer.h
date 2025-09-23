#ifndef GALAY_UDP_SERVER_H
#define GALAY_UDP_SERVER_H 

#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "ServerDefn.hpp"

namespace galay
{
    class UdpServer
    { 
    public:
        UdpServer();
        UdpServer(Runtime&& runtime);
        UdpServer(UdpServer&& server);
        UdpServer(const UdpServer& server) = delete;
        void listenOn(Host host);
        void useStrategy(ServerStrategy strategy);
        void run(const std::function<Coroutine<nil>(AsyncUdpSocket,AsyncFactory)>& callback);
        void stop();
        void wait();
        UdpServer& operator=(UdpServer&& server);
        UdpServer& operator=(const UdpServer& server) = delete;
        ~UdpServer();
    protected:
        Host m_host = {"0.0.0.0", 8080};
        Runtime m_runtime;
        std::mutex m_mutex;
        std::condition_variable m_condition;
        ServerStrategy m_strategy = ServerStrategy::SingleRuntime;
    };

    class UdpServerBuilder
    {
    public:
        UdpServerBuilder& addListen(const Host& host);
        UdpServerBuilder& startCoChecker(std::chrono::milliseconds interval);
        UdpServer build();
        UdpServerBuilder& strategy(ServerStrategy strategy);
        /*
            @brief EventScheduler timeout
            @param timeout milliseconds, -1 means never timeout
        */
        UdpServerBuilder& timeout(int timeout);
        UdpServerBuilder& threads(int threads);
    private:
        Host m_host = {"0.0.0.0", 8080};
        std::chrono::milliseconds m_coCheckerInterval = std::chrono::milliseconds(-1);
        int m_threads = DEFAULT_COS_SCHEDULER_THREAD_NUM;
        int m_timeout = -1;
        ServerStrategy m_strategy = ServerStrategy::SingleRuntime;
    };
}

#endif