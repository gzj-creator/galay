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
        TcpClient(AsyncTcpSocket&& socket);
        TcpClient(Runtime& runtime, const Host& bind_addr);
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        TcpClient cloneForDifferentRole(Runtime& runtime) const;
    private:
        AsyncTcpSocket m_socket;
    };
    
}

#endif