#ifndef GALAY_UDP_SERVER_H
#define GALAY_UDP_SERVER_H 

#include "galay/kernel/async/Socket.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"

namespace galay
{
    class UdpServer
    {
        friend class UdpServerBuilder;
    public:
        UdpServer();
        UdpServer(Runtime&& runtime);
        UdpServer(UdpServer&& server);
        UdpServer(const UdpServer& server) = delete;
        void run(const std::function<Coroutine<nil>(AsyncUdpSocket&)>& callback);
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
        std::vector<AsyncUdpSocket> m_sockets;
    };

    class UdpServerBuilder
    {
    public:
        UdpServerBuilder& addListen(const Host& host);
        UdpServerBuilder& startCoChecker(bool start, std::chrono::milliseconds interval);
        UdpServer build();
    private:
        UdpServer m_server;
    };
}

#endif