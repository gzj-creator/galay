#include "UdpClient.h"

namespace galay 
{
    UdpClient::UdpClient(Runtime &runtime)
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

    UdpClient::UdpClient(AsyncUdpSocket &&socket)
        : m_socket(std::move(socket))
    {
    }

    UdpClient::UdpClient(Runtime &runtime, const Host &bind_addr)
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

    AsyncResult<std::expected<void, CommonError>> UdpClient::connect(const Host& addr)
    {
        return m_socket.connect(addr);
    }

    AsyncResult<std::expected<Bytes, CommonError>> UdpClient::recv(char* buffer, size_t length)
    {
        return m_socket.recv(buffer, length);
    }

    AsyncResult<std::expected<Bytes, CommonError>> UdpClient::send(Bytes bytes)
    {
        return m_socket.send(std::move(bytes));
    }

    AsyncResult<std::expected<Bytes, CommonError>> UdpClient::recvfrom(Host& remote, char* buffer, size_t length)
    {
        return m_socket.recvfrom(remote, buffer, length);
    }

    AsyncResult<std::expected<Bytes, CommonError>> UdpClient::sendto(const Host& remote, Bytes bytes)
    {
        return m_socket.sendto(remote, std::move(bytes));
    }

    AsyncResult<std::expected<void, CommonError>> UdpClient::close()
    {
        return m_socket.close();
    }

    UdpClient UdpClient::alsoRunningOn(Runtime& runtime) const
    {
        return UdpClient(m_socket.alsoRunningOn(runtime));
    }
}