#ifndef GALAY_KERNEL_TCP_SERVER_H
#define GALAY_KERNEL_TCP_SERVER_H 

#include "galay/kernel/async/Socket.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"

namespace galay 
{ 
    #define DEFAULT_TCP_BACKLOG_SIZE       1024

    class TcpServer
    {
        friend class TcpServerBuilder;
    public:
        TcpServer();
        TcpServer(Runtime&& runtime);
        TcpServer(TcpServer&& server);
        TcpServer(const TcpServer& server) = delete;
        void run(const std::function<Coroutine<nil>(AsyncTcpSocket)>& callback);
        void stop();
        void wait();
        TcpServer& operator=(TcpServer&& server);
        TcpServer& operator=(const TcpServer& server) = delete;
        ~TcpServer();
    private:
        Coroutine<nil> acceptConnection(const std::function<Coroutine<nil>(AsyncTcpSocket)>& callback, size_t i);
    protected:
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        Host m_host = {"0.0.0.0", 8080};
        Runtime m_runtime;
        std::mutex m_mutex;
        std::condition_variable m_condition;
        std::vector<AsyncTcpSocket> m_sockets;
    };

    class TcpServerBuilder
    {
    public:
        TcpServerBuilder& addListen(const Host& host);
        TcpServerBuilder& backlog(int backlog);
        TcpServerBuilder& startCoChecker(bool start, std::chrono::milliseconds interval);
        TcpServer build();
    private:
        TcpServer m_server;
    };


}


#endif