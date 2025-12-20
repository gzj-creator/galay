#ifndef GALAY_UDP_CLIENT_H
#define GALAY_UDP_CLIENT_H 

#include "galay/kernel/async/Socket.h"

namespace galay 
{
    class UdpClient
    {
    public:
        //default constructor
        UdpClient() = default;
        //throw exception
        UdpClient(CoSchedulerHandle handle);
        UdpClient(AsyncUdpSocket&& socket);
        UdpClient(CoSchedulerHandle handle, const Host& bind_addr);
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        AsyncResult<std::expected<Bytes, CommonError>> recvfrom(Host& remote, char* buffer, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> sendto(const Host& remote, Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        UdpClient cloneForDifferentRole(CoSchedulerHandle handle) const;
    private:
        AsyncUdpSocket m_socket;
    };
}

#endif