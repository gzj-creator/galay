#ifndef GALAY_TCP_SSL_CLIENT_H
#define GALAY_TCP_SSL_CLIENT_H 

#include <openssl/ssl.h>
#include "galay/kernel/async/Socket.h"

namespace galay 
{ 
    class TcpSslClient 
    {
    public:
        //throw exception - creates own SSL_CTX
        TcpSslClient(Runtime& runtime, const char* server_pem = nullptr);
        //throw exception - uses provided SSL_CTX
        TcpSslClient(Runtime& runtime, SSL_CTX* ssl_ctx);
        TcpSslClient(AsyncSslSocket&& socket);
        TcpSslClient(Runtime& runtime, const Host& bind_addr, const char* server_pem = nullptr);
        TcpSslClient(Runtime& runtime, const Host& bind_addr, SSL_CTX* ssl_ctx);
        ~TcpSslClient();
        
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        void readyToSslConnect();
        AsyncResult<std::expected<bool, CommonError>> sslConnect();
        AsyncResult<std::expected<Bytes, CommonError>> sslRecv(char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> sslSend(Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        TcpSslClient cloneForDifferentRole(Runtime& runtime) const;
    private:
        bool initSSLContext(const char* server_pem);
    private:
        AsyncSslSocket m_socket;
        SSL_CTX* m_ssl_ctx = nullptr;
        bool m_owns_ctx = false;  // Whether we own the SSL_CTX and should free it
    };
}

#endif