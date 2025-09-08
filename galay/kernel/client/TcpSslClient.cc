#include "TcpSslClient.h"

namespace galay
{
    TcpSslClient::TcpSslClient(Runtime &runtime)
        :m_socket(runtime)
    {
        ValueWrapper<bool> res = m_socket.socket();
        if(!res.success()) {
            throw std::runtime_error(res.getError()->message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
    }

    TcpSslClient::TcpSslClient(Runtime &runtime, const Host &bind_addr) 
        :m_socket(runtime)
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

    AsyncResult<ValueWrapper<bool>> TcpSslClient::connect(const Host& addr)
    {
        return m_socket.sslConnect(addr);
    }

    AsyncResult<ValueWrapper<Bytes>> TcpSslClient::recv(char* buffer, size_t length)
    {
        return m_socket.sslRecv(buffer, length);
    }

    AsyncResult<ValueWrapper<Bytes>> TcpSslClient::send(Bytes bytes)
    {
        return m_socket.sslSend(std::move(bytes));
    }

    AsyncResult<ValueWrapper<bool>> TcpSslClient::close()
    {
        return m_socket.sslClose();
    }
}