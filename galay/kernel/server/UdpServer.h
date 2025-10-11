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
        using AsyncUdpFunc = std::function<Coroutine<nil>(AsyncUdpSocket)>;
        UdpServer() {}
        UdpServer(UdpServer&& server);
        UdpServer(const UdpServer& server) = delete;
        void listenOn(Host host);
        void run(Runtime& runtime, const AsyncUdpFunc& callback);
        void stop();
        void wait();
        UdpServer& operator=(UdpServer&& server);
        UdpServer& operator=(const UdpServer& server) = delete;
        ~UdpServer() {}
    protected:
        Host m_host = {"0.0.0.0", 8080};
        std::mutex m_mutex;
        std::condition_variable m_condition;
    };

    class UdpServerBuilder
    {
    public:
        UdpServerBuilder& addListen(const Host& host);
        UdpServer build();
    private:
        Host m_host = {"0.0.0.0", 8080};
    };
}

#endif