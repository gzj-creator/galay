#ifndef GALAY_UDP_CLIENT_H
#define GALAY_UDP_CLIENT_H 

#include "galay/kernel/async/Socket.h"

namespace galay 
{
    class UdpClient
    {
    public:
        //throw exception
        UdpClient(Runtime& runtime);
        UdpClient(Runtime& runtime, const Host& bind_addr);
        AsyncResult<ValueWrapper<bool>> connect(const Host& addr);
        AsyncResult<ValueWrapper<Bytes>> recv(char* buffer, size_t length);
        AsyncResult<ValueWrapper<Bytes>> send(Bytes bytes);
        AsyncResult<ValueWrapper<Bytes>> recvfrom(Host& remote, char* buffer, size_t length);
        AsyncResult<ValueWrapper<Bytes>> sendto(const Host& remote, Bytes bytes);
        AsyncResult<ValueWrapper<bool>> close();
    private:
        AsyncUdpSocket m_socket;
    };
}

#endif