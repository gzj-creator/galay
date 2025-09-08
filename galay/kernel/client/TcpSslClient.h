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
        AsyncResult<ValueWrapper<bool>> connect(const Host& addr);
        AsyncResult<ValueWrapper<Bytes>> recv(char* buffer, size_t length);
        AsyncResult<ValueWrapper<Bytes>> send(Bytes bytes);
        AsyncResult<ValueWrapper<bool>> close();
    private:
        AsyncSslSocket m_socket;
    };
}

#endif