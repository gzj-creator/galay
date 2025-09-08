#include "TcpClient.h"

namespace galay 
{
    TcpClient::TcpClient(Runtime &runtime)
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

    TcpClient::TcpClient(Runtime &runtime, const Host &bind_addr)
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

    AsyncResult<ValueWrapper<bool>> TcpClient::connect(const Host& addr)
    {
        return m_socket.connect(addr);
    }

    AsyncResult<ValueWrapper<Bytes>> TcpClient::recv(char* buffer, size_t length)
    {
        return m_socket.recv(buffer, length);
    }

    AsyncResult<ValueWrapper<Bytes>> TcpClient::send(Bytes bytes)
    {
        return m_socket.send(std::move(bytes));
    }

    AsyncResult<ValueWrapper<bool>> TcpClient::close()
    {
        return m_socket.close();
    }
}