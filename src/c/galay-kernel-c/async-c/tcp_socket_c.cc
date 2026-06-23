#include "tcp_socket_c.h"

#include "../../../cpp/galay-kernel/async/tcp_socket.h"

#include <cstring>
#include <new>
#include <string>
#include <utility>

bool is_valid_c_ip_type(C_IPType ip_type) {
    return ip_type == IPV4 || ip_type == IPV6;
}

galay::kernel::IPType from_c_ip_type_to_cpp_ip_type(C_IPType ip_type) {
    switch (ip_type)
    {
    case IPV4:
        return galay::kernel::IPType::IPV4;
    case IPV6:
        return galay::kernel::IPType::IPV6;
    }
    return galay::kernel::IPType::IPV4;
}

std::string from_c_host_address_to_string(const C_Host& host)
{
    const void* end = std::memchr(host.address, '\0', sizeof(host.address));
    const auto length = end == nullptr
        ? sizeof(host.address)
        : static_cast<size_t>(static_cast<const char*>(end) - host.address);
    return std::string(host.address, length);
}

galay::kernel::Host from_c_host_to_cpp_host(const C_Host& host)
{
    return galay::kernel::Host(
        static_cast<galay::kernel::IPType>(host.type),
        from_c_host_address_to_string(host),
        host.port);
}

C_TcpSocketResultCode galay_kernel_tcp_socket_create(galay_kernel_tcp_socket_t* c_socket, C_IPType type)
{
    if (c_socket == nullptr || !is_valid_c_ip_type(type))
    {
        return ParameterInvalid;
    }

    c_socket->socket = nullptr;
    auto result = galay::async::TcpSocket::create(from_c_ip_type_to_cpp_ip_type(type));
    if (!result)
    {
        return MemoryAllocFailed;
    }

    auto socket = new (std::nothrow) galay::async::TcpSocket(std::move(*result));
    if (socket == nullptr)
    {
        return MemoryAllocFailed;
    }

    c_socket->socket = socket;
    return Success;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket)
{
    if (c_socket == nullptr) return MemoryAllocFailed;
    delete static_cast<galay::async::TcpSocket*>(c_socket->socket);
    return Success;
}


C_TcpSocketResultCode galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || host == nullptr)
    {
        return ParameterInvalid;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto reuse_addr = socket->option().handleReuseAddr();
    if (!reuse_addr)
    {
        return IOFailed;
    }
    auto non_block = socket->option().handleNonBlock();
    if (!non_block)
    {
        return IOFailed;
    }
    auto bound = socket->bind(from_c_host_to_cpp_host(*host));
    if (!bound)
    {
        return galay::kernel::IOError::contains(bound.error().code(), galay::kernel::kParamInvalid)
            ? ParameterInvalid
            : IOFailed;
    }
    return Success;
}
