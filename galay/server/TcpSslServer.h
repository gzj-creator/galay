#ifndef GALAY_TCP_SSL_SERVER_H
#define GALAY_TCP_SSL_SERVER_H 

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <openssl/ssl.h>
#include "galay/kernel/async/Socket.h"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "ServerDefn.hpp"

namespace galay
{

    // 保证单进程单例
    class TcpSslServer
    {
    public:
        using AsyncSslFunc = std::function<Coroutine<nil>(AsyncSslSocket, CoSchedulerHandle)>;
        TcpSslServer(const std::string& cert_file, const std::string& key_file);
        TcpSslServer(TcpSslServer&& server);
        TcpSslServer(const TcpSslServer& server) = delete;
        void listenOn(Host host, int backlog);
        void run(Runtime& runtime, const AsyncSslFunc& callback);
        void stop();
        void wait();
        
        /**
         * @brief 主动初始化SSL上下文
         * @return 初始化成功返回true，失败或已初始化返回false
         */
        bool initializeSSLContext();
        
        /**
         * @brief 获取SSL上下文指针
         * @return SSL_CTX指针，如果未初始化则返回nullptr
         */
        SSL_CTX* getSSLContext() const;
        
        TcpSslServer& operator=(TcpSslServer&& server);
        TcpSslServer& operator=(const TcpSslServer& server) = delete;
        ~TcpSslServer();
    private:
        Coroutine<nil> acceptConnection(CoSchedulerHandle handle, AsyncSslFunc callback, size_t i);
        bool initSSLContext();
    protected:
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        Host m_host = {"0.0.0.0", 8080};
        std::string m_cert;
        std::string m_key;
        SSL_CTX* m_ssl_ctx = nullptr;
        std::mutex m_mutex;
        std::condition_variable m_condition;
        std::vector<AsyncSslSocket> m_sockets;
        std::atomic<bool> m_running{false};
    };

    class TcpSslServerBuilder
    {
    public:
        TcpSslServerBuilder(const std::string& cert, const std::string& key);
        TcpSslServerBuilder& addListen(const Host& host);
        TcpSslServerBuilder& backlog(int backlog);
        TcpSslServer build();
    private:
        std::string m_cert = "server.crt";
        std::string m_key = "server.key";
        Host m_host = {"0.0.0.0", 8080};
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
    };
}


#endif