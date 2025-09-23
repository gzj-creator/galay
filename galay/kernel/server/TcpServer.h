#ifndef GALAY_KERNEL_TCP_SERVER_H
#define GALAY_KERNEL_TCP_SERVER_H 

#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "ServerDefn.hpp"

namespace galay 
{
    class TcpServer
    {
    public:
        TcpServer();
        TcpServer(Runtime&& runtime);
        TcpServer(TcpServer&& server);
        TcpServer(const TcpServer& server) = delete;
        void listenOn(Host host, int backlog);
        void useStrategy(ServerStrategy strategy);
        bool run(const std::function<Coroutine<nil>(AsyncTcpSocket,AsyncFactory)>& callback);
        void stop();
        void wait();
        TcpServer& operator=(TcpServer&& server);
        TcpServer& operator=(const TcpServer& server) = delete;
        ~TcpServer();
    private:
        Coroutine<nil> acceptConnection(const std::function<Coroutine<nil>(AsyncTcpSocket,AsyncFactory)>& callback, size_t i);
    protected:
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        Host m_host = {"0.0.0.0", 8080};
        Runtime m_runtime;
        std::mutex m_mutex;
        std::condition_variable m_condition;
        std::vector<AsyncTcpSocket> m_sockets;
        ServerStrategy m_strategy = ServerStrategy::SingleRuntime;
    };

    

    class TcpServerBuilder
    {
    public:
        
        TcpServerBuilder& addListen(const Host& host);
        TcpServerBuilder& backlog(int backlog);
        TcpServerBuilder& startCoChecker(std::chrono::milliseconds interval);
        TcpServerBuilder& strategy(ServerStrategy strategy);
        /*
            @brief EventScheduler timeout
            @param timeout milliseconds, -1 means never timeout
        */
        TcpServerBuilder& timeout(int timeout);
        TcpServerBuilder& threads(int threads);
        TcpServer build();
    private:
        Host m_host = {"0.0.0.0", 8080};
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        std::chrono::milliseconds m_coCheckerInterval = std::chrono::milliseconds(-1);
        int m_threads = DEFAULT_COS_SCHEDULER_THREAD_NUM;
        int m_timeout = -1;
        ServerStrategy m_strategy = ServerStrategy::SingleRuntime;
    };


}


#endif