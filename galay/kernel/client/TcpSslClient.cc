#include "TcpSslClient.h"

namespace galay
{
    TcpSslClient::TcpSslClient(Runtime &runtime)
        :m_socket(runtime)
    {
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
    }

    TcpSslClient::TcpSslClient(AsyncSslSocket &&socket)
        :m_socket(std::move(socket))
    {
    }

    TcpSslClient::TcpSslClient(Runtime &runtime, const Host &bind_addr) 
        :m_socket(runtime)
    {
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
        std::expected<void, CommonError> bind_res = m_socket.bind(bind_addr);
        if(!bind_res) {
            throw std::runtime_error(bind_res.error().message());
        }
    }

    AsyncResult<std::expected<void, CommonError>> TcpSslClient::connect(const Host& addr)
    {
        return m_socket.sslConnect(addr);
    }

    AsyncResult<std::expected<Bytes, CommonError>> TcpSslClient::recv(char* buffer, size_t length)
    {
        return m_socket.sslRecv(buffer, length);
    }

    AsyncResult<std::expected<Bytes, CommonError>> TcpSslClient::send(Bytes bytes)
    {
        return m_socket.sslSend(std::move(bytes));
    }

    AsyncResult<std::expected<void, CommonError>> TcpSslClient::close()
    {
        return m_socket.sslClose();
    }

    TcpSslClient TcpSslClient::alsoRunningOn(Runtime& runtime) const
    {
        return TcpSslClient(m_socket.alsoRunningOn(runtime));
    }
}