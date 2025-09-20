#ifndef GALAY_TCP_SSL_CLIENT_H
#define GALAY_TCP_SSL_CLIENT_H 

#include "galay/kernel/async/Socket.h"

namespace galay 
{ 
    class TcpSslClient 
    {
    public:
        //throw exception
        TcpSslClient(Runtime& runtime);
        TcpSslClient(AsyncSslSocket&& socket);
        TcpSslClient(Runtime& runtime, const Host& bind_addr);
        
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        void readyToSslConnect();
        AsyncResult<std::expected<bool, CommonError>> sslConnect();
        AsyncResult<std::expected<Bytes, CommonError>> sslRecv(char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> sslSend(Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        TcpSslClient alsoRunningOn(Runtime& runtime) const;
    private:
        AsyncSslSocket m_socket;
    };
}

#endif