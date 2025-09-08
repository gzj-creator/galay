#ifndef GALAY_TCP_CLIENT_H
#define GALAY_TCP_CLIENT_H 

#include "galay/kernel/async/Socket.h"

namespace galay 
{
    class TcpClient 
    {
    public:
        //throw exception
        TcpClient(Runtime& runtime);
        TcpClient(Runtime& runtime, const Host& bind_addr);
        AsyncResult<ValueWrapper<bool>> connect(const Host& addr);
        AsyncResult<ValueWrapper<Bytes>> recv(char* buffer, size_t length);
        AsyncResult<ValueWrapper<Bytes>> send(Bytes bytes);
        AsyncResult<ValueWrapper<bool>> close();
    private:
        AsyncTcpSocket m_socket;
    };
    
}

#endif