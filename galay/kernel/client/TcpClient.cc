#include "TcpClient.h"

namespace galay 
{
    TcpClient::TcpClient(Runtime &runtime)
        : m_socket(runtime)
    {
        std::expected<void, CommonError> res = m_socket.socket();
        if(!res) {
            throw std::runtime_error(res.error().message());
        }
        if(!m_socket.options().handleNonBlock()) {
            throw std::runtime_error("set socket non-block error");
        }
    }

    TcpClient::TcpClient(AsyncTcpSocket &&socket)
        : m_socket(std::move(socket))
    {
    }

    TcpClient::TcpClient(Runtime &runtime, const Host &bind_addr)
        : m_socket(runtime)
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

    AsyncResult<std::expected<void, CommonError>> TcpClient::connect(const Host& addr)
    {
        return m_socket.connect(addr);
    }

    AsyncResult<std::expected<Bytes, CommonError>> TcpClient::recv(char* buffer, size_t length)
    {
        return m_socket.recv(buffer, length);
    }

    AsyncResult<std::expected<Bytes, CommonError>> TcpClient::send(Bytes bytes)
    {
        return m_socket.send(std::move(bytes));
    }

    AsyncResult<std::expected<void, CommonError>> TcpClient::close()
    {
        return m_socket.close();
    }

    TcpClient TcpClient::cloneForDifferentRole(Runtime& runtime) const
    {
        return TcpClient(m_socket.cloneForDifferentRole(runtime));
    }
}