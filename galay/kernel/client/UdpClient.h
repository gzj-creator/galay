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
        UdpClient(AsyncUdpSocket&& socket);
        UdpClient(Runtime& runtime, const Host& bind_addr);
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        AsyncResult<std::expected<Bytes, CommonError>> recvfrom(Host& remote, char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> sendto(const Host& remote, Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        UdpClient cloneForDifferentRole(Runtime& runtime) const;
    private:
        AsyncUdpSocket m_socket;
    };
}

#endif