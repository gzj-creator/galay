#ifndef GALAY_TCP_SSL_SERVER_H
#define GALAY_TCP_SSL_SERVER_H 

#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/CoScheduler.hpp"

namespace galay
{
    #define DEFAULT_TCP_BACKLOG_SIZE       1024

    // 保证单进程单例
    class TcpSslServer
    {
        friend class TcpSslServerBuilder;
    public:
        TcpSslServer();
        TcpSslServer(Runtime&& runtime);
        TcpSslServer(TcpSslServer&& server);
        TcpSslServer(const TcpSslServer& server) = delete;
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
    };

    class TcpSslServerBuilder
    {
    public:
        TcpSslServerBuilder& sslConf(const std::string& cert, const std::string& key);
        TcpSslServerBuilder& addListen(const Host& host);
        TcpSslServerBuilder& backlog(int backlog);
        TcpSslServerBuilder& startCoChecker(bool start, std::chrono::milliseconds interval);
        TcpSslServer build();
    private:
        TcpSslServer m_server;
    };
}


#endif