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
        using AsyncTcpFunc = std::function<Coroutine<nil>(AsyncTcpSocket)>;
        TcpServer() {}
        TcpServer(TcpServer&& server);
        TcpServer(const TcpServer& server) = delete;
        void listenOn(Host host, int backlog);
        bool run(Runtime& runtime, const AsyncTcpFunc& callback);
        void stop();
        void wait();
        TcpServer& operator=(TcpServer&& server);
        TcpServer& operator=(const TcpServer& server) = delete;
        ~TcpServer() {}
    private:
        Coroutine<nil> acceptConnection(Runtime& runtime, const AsyncTcpFunc& callback, size_t i);
    protected:
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        Host m_host = {"0.0.0.0", 8080};
        std::mutex m_mutex;
        std::condition_variable m_condition;
        std::vector<AsyncTcpSocket> m_sockets;
    };

    

    class TcpServerBuilder
    {
    public:
        
        TcpServerBuilder& addListen(const Host& host);
        TcpServerBuilder& backlog(int backlog);
        TcpServer build();
    private:
        Host m_host = {"0.0.0.0", 8080};
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
    };


}


#endif