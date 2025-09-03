#include "UdpClient.h"

namespace galay 
{
    UdpClient::UdpClient(Runtime &runtime)
        : m_socket(runtime)
    {
        ValueWrapper<bool> res = m_socket.socket();
        if(!res.success()) {
            throw std::runtime_error(res.getError()->message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
    }

    UdpClient::UdpClient(Runtime &runtime, const Host &bind_addr)
        : m_socket(runtime)
    {
        ValueWrapper<bool> res = m_socket.socket();
        if(!res.success()) {
            throw std::runtime_error(res.getError()->message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
        ValueWrapper<bool> bind_res = m_socket.bind(bind_addr);
        if(!bind_res.success()) {
            throw std::runtime_error(bind_res.getError()->message());
        }
    }

    AsyncResult<ValueWrapper<bool>> UdpClient::connect(const Host& addr)
    {
        return m_socket.connect(addr);
    }

    AsyncResult<ValueWrapper<Bytes>> UdpClient::recv(size_t length)
    {
        return m_socket.recv(length);
    }

    AsyncResult<ValueWrapper<Bytes>> UdpClient::send(Bytes bytes)
    {
        return m_socket.send(std::move(bytes));
    }

    AsyncResult<ValueWrapper<Bytes>> UdpClient::recvfrom(Host& remote, size_t length)
    {
        return m_socket.recvfrom(remote, length);
    }

    AsyncResult<ValueWrapper<Bytes>> UdpClient::sendto(const Host& remote, Bytes bytes)
    {
        return m_socket.sendto(remote, std::move(bytes));
    }

    AsyncResult<ValueWrapper<bool>> UdpClient::close()
    {
        return m_socket.close();
    }
}