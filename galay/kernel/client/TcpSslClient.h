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
        TcpSslClient(Runtime& runtime, const Host& bind_addr);
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
    private:
        AsyncSslSocket m_socket;
    };
}

#endif