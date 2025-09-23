#ifndef GALAY_TCP_SSL_SERVER_H
#define GALAY_TCP_SSL_SERVER_H 

#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"
#include "ServerDefn.hpp"

namespace galay
{
    #define DEFAULT_TCP_BACKLOG_SIZE       1024

    // 保证单进程单例
    class TcpSslServer
    {
    public:
        TcpSslServer(const std::string& cert_file, const std::string& key_file);
        TcpSslServer(Runtime&& runtime, const std::string& cert_file, const std::string& key_file);
        TcpSslServer(TcpSslServer&& server);
        TcpSslServer(const TcpSslServer& server) = delete;
        void listenOn(Host host, int backlog);
        void useStrategy(ServerStrategy strategy);
        void run(const std::function<Coroutine<nil>(AsyncSslSocket,AsyncFactory)>& callback);
        void stop();
        void wait();
        TcpSslServer& operator=(TcpSslServer&& server);
        TcpSslServer& operator=(const TcpSslServer& server) = delete;
        ~TcpSslServer();
    private:
        Coroutine<nil> acceptConnection(const std::function<Coroutine<nil>(AsyncSslSocket,AsyncFactory)>& callback, size_t i);
    protected:
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        Host m_host = {"0.0.0.0", 8080};
        std::string m_cert;
        std::string m_key;
        Runtime m_runtime;
        std::mutex m_mutex;
        std::condition_variable m_condition;
        std::vector<AsyncSslSocket> m_sockets;
        ServerStrategy m_strategy = ServerStrategy::SingleRuntime;
    };

    class TcpSslServerBuilder
    {
    public:
        TcpSslServerBuilder(const std::string& cert, const std::string& key);
        TcpSslServerBuilder& addListen(const Host& host);
        TcpSslServerBuilder& backlog(int backlog);
        TcpSslServerBuilder& startCoChecker(std::chrono::milliseconds interval);
        TcpSslServerBuilder& strategy(ServerStrategy strategy);
        /*
            @brief EventScheduler timeout
            @param timeout milliseconds, -1 means never timeout
        */
        TcpSslServerBuilder& timeout(int timeout);
        TcpSslServerBuilder& threads(int threads);
        TcpSslServer build();
    private:
        std::string m_cert = "server.crt";
        std::string m_key = "server.key";
        Host m_host = {"0.0.0.0", 8080};
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        std::chrono::milliseconds m_coCheckerInterval = std::chrono::milliseconds(-1);
        int m_threads = DEFAULT_COS_SCHEDULER_THREAD_NUM;
        int m_timeout = -1;
        ServerStrategy m_strategy = ServerStrategy::SingleRuntime;
    };
}


#endif