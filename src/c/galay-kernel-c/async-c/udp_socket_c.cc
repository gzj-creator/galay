#include "udp_socket_c.h"

#include "../../../cpp/galay-kernel/async/udp_socket.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <new>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace
{

bool is_valid_c_ip_type(C_IPType ip_type)
{
    return ip_type == C_IPTypeIPV4 || ip_type == C_IPTypeIPV6;
}

galay::kernel::IPType from_c_ip_type_to_cpp_ip_type(C_IPType ip_type)
{
    switch (ip_type)
    {
    case C_IPTypeIPV4:
        return galay::kernel::IPType::IPV4;
    case C_IPTypeIPV6:
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

bool assign_c_host_from_sockaddr(const sockaddr_storage& storage, C_Host* endpoint)
{
    C_Host out{};
    if (storage.ss_family == AF_INET)
    {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, out.address, sizeof(out.address)) == nullptr)
        {
            return false;
        }
        out.type = C_IPTypeIPV4;
        out.port = ntohs(addr->sin_port);
        *endpoint = out;
        return true;
    }
    if (storage.ss_family == AF_INET6)
    {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, out.address, sizeof(out.address)) == nullptr)
        {
            return false;
        }
        out.type = C_IPTypeIPV6;
        out.port = ntohs(addr->sin6_port);
        *endpoint = out;
        return true;
    }
    return false;
}

bool assign_c_host_from_cpp_host(const galay::kernel::Host& host, C_Host* endpoint)
{
    if (endpoint == nullptr || !host.valid())
    {
        return false;
    }

    const auto address = host.ip();
    if (address.empty() || address.size() >= sizeof(endpoint->address))
    {
        return false;
    }

    C_Host out{};
    out.type = host.isIPv4() ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memcpy(out.address, address.c_str(), address.size() + 1);
    out.port = host.port();
    *endpoint = out;
    return true;
}

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return static_cast<galay::kernel::Runtime*>(runtime->runtime);
}

C_UdpSocketResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    return galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid)
        ? C_UdpSocketParameterInvalid
        : C_UdpSocketIOFailed;
}

bool data_buffer_valid(const void* buffer, size_t length)
{
    return length == 0 || buffer != nullptr;
}

galay::kernel::Task<void> c_api_recvfrom(
    galay::async::UdpSocket* socket,
    char* buffer,
    size_t length,
    galay_kernel_udp_recvfrom_callback_t callback,
    void* ctx)
{
    galay::kernel::Host from;
    auto received = co_await socket->recvfrom(buffer, length, &from);

    galay_kernel_udp_recvfrom_result_t result{};
    result.buffer = buffer;
    result.length = length;
    if (!received)
    {
        result.code = from_cpp_io_error(received.error());
        callback(&result, ctx);
        co_return;
    }

    result.bytes = *received;
    result.code = assign_c_host_from_cpp_host(from, &result.from)
        ? C_UdpSocketSuccess
        : C_UdpSocketIOFailed;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recvfrom_loop(
    galay::async::UdpSocket* socket,
    char* buffer,
    size_t length,
    galay_kernel_udp_recvfrom_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        galay::kernel::Host from;
        auto received = co_await socket->recvfrom(buffer, length, &from);

        galay_kernel_udp_recvfrom_result_t result{};
        result.buffer = buffer;
        result.length = length;
        if (!received)
        {
            result.code = from_cpp_io_error(received.error());
            (void)callback(&result, ctx);
            co_return;
        }

        result.bytes = *received;
        result.code = assign_c_host_from_cpp_host(from, &result.from)
            ? C_UdpSocketSuccess
            : C_UdpSocketIOFailed;
        const int should_stop = callback(&result, ctx);
        if (should_stop != 0 || result.code != C_UdpSocketSuccess)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_sendto(
    galay::async::UdpSocket* socket,
    const char* buffer,
    size_t length,
    galay::kernel::Host to,
    C_Host c_to,
    galay_kernel_udp_sendto_callback_t callback,
    void* ctx)
{
    auto sent = co_await socket->sendto(buffer, length, to);
    galay_kernel_udp_sendto_result_t result{};
    result.code = sent ? C_UdpSocketSuccess : from_cpp_io_error(sent.error());
    result.to = c_to;
    result.buffer = buffer;
    result.length = length;
    result.bytes = sent ? *sent : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_sendto_loop(
    galay::async::UdpSocket* socket,
    const char* buffer,
    size_t length,
    galay::kernel::Host to,
    C_Host c_to,
    galay_kernel_udp_sendto_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        auto sent = co_await socket->sendto(buffer, length, to);
        galay_kernel_udp_sendto_result_t result{};
        result.code = sent ? C_UdpSocketSuccess : from_cpp_io_error(sent.error());
        result.to = c_to;
        result.buffer = buffer;
        result.length = length;
        result.bytes = sent ? *sent : 0;
        const int should_stop = callback(&result, ctx);
        if (should_stop != 0 || !sent)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_close(
    galay::async::UdpSocket* socket,
    galay_kernel_udp_close_callback_t callback,
    void* ctx)
{
    auto closed = co_await socket->close();
    callback(closed ? C_UdpSocketSuccess : from_cpp_io_error(closed.error()), ctx);
    co_return;
}

} // namespace

const char* galay_kernel_udp_socket_get_error(C_UdpSocketResultCode code)
{
    switch (code)
    {
    case C_UdpSocketSuccess:
        return "success";
    case C_UdpSocketParameterInvalid:
        return "parameter invalid";
    case C_UdpSocketMemoryAllocFailed:
        return "memory allocation failed";
    case C_UdpSocketIOFailed:
        return "io failed";
    case C_UdpSocketOperationInvalid:
        return "operation invalid";
    case C_UdpSocketRuntimeNotRunning:
        return "runtime not running";
    case C_UdpSocketRuntimeSpawnFailed:
        return "runtime spawn failed";
    }
    return "unknown udp socket error";
}

C_UdpSocketResultCode galay_kernel_udp_socket_create(galay_kernel_udp_socket_t* c_socket, C_IPType type)
{
    if (c_socket == nullptr || !is_valid_c_ip_type(type))
    {
        return C_UdpSocketParameterInvalid;
    }

    c_socket->socket = nullptr;
    auto result = galay::async::UdpSocket::create(from_c_ip_type_to_cpp_ip_type(type));
    if (!result)
    {
        return from_cpp_io_error(result.error());
    }

    auto* socket = new (std::nothrow) galay::async::UdpSocket(std::move(*result));
    if (socket == nullptr)
    {
        return C_UdpSocketMemoryAllocFailed;
    }

    c_socket->socket = socket;
    return C_UdpSocketSuccess;
}

C_UdpSocketResultCode galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t* c_socket)
{
    if (c_socket == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }
    delete static_cast<galay::async::UdpSocket*>(c_socket->socket);
    c_socket->socket = nullptr;
    return C_UdpSocketSuccess;
}

C_UdpSocketResultCode galay_kernel_udp_socket_bind(
    galay_kernel_udp_socket_t* c_socket,
    const C_Host* host)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || host == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    auto cpp_host = from_c_host_to_cpp_host(*host);
    if (!cpp_host.valid())
    {
        return C_UdpSocketParameterInvalid;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto reuse_addr = socket->option().handleReuseAddr();
    if (!reuse_addr)
    {
        return C_UdpSocketIOFailed;
    }
    auto non_block = socket->option().handleNonBlock();
    if (!non_block)
    {
        return C_UdpSocketIOFailed;
    }
    auto bound = socket->bind(cpp_host);
    if (!bound)
    {
        return from_cpp_io_error(bound.error());
    }
    return C_UdpSocketSuccess;
}

C_UdpSocketResultCode galay_kernel_udp_socket_local_endpoint(
    const galay_kernel_udp_socket_t* c_socket,
    C_Host* endpoint)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || endpoint == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    const auto* socket = static_cast<const galay::async::UdpSocket*>(c_socket->socket);
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(socket->handle().fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
    {
        return C_UdpSocketIOFailed;
    }

    return assign_c_host_from_sockaddr(storage, endpoint) ? C_UdpSocketSuccess : C_UdpSocketIOFailed;
}

C_UdpSocketResultCode galay_kernel_udp_socket_recvfrom(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_udp_recvfrom_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        !data_buffer_valid(buffer, length) || callback == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UdpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_recvfrom(socket, buffer, length, callback, ctx));
    return spawned ? C_UdpSocketSuccess : C_UdpSocketRuntimeSpawnFailed;
}

C_UdpSocketResultCode galay_kernel_udp_socket_recvfrom_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_udp_recvfrom_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        !data_buffer_valid(buffer, length) || callback == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UdpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_recvfrom_loop(socket, buffer, length, callback, ctx));
    return spawned ? C_UdpSocketSuccess : C_UdpSocketRuntimeSpawnFailed;
}

C_UdpSocketResultCode galay_kernel_udp_socket_sendto(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    galay_kernel_udp_sendto_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        !data_buffer_valid(buffer, length) || to == nullptr || callback == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    auto cpp_to = from_c_host_to_cpp_host(*to);
    if (!cpp_to.valid())
    {
        return C_UdpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UdpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_sendto(socket, buffer, length, cpp_to, *to, callback, ctx));
    return spawned ? C_UdpSocketSuccess : C_UdpSocketRuntimeSpawnFailed;
}

C_UdpSocketResultCode galay_kernel_udp_socket_sendto_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    const C_Host* to,
    galay_kernel_udp_sendto_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        !data_buffer_valid(buffer, length) || to == nullptr || callback == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    auto cpp_to = from_c_host_to_cpp_host(*to);
    if (!cpp_to.valid())
    {
        return C_UdpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UdpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_sendto_loop(socket, buffer, length, cpp_to, *to, callback, ctx));
    return spawned ? C_UdpSocketSuccess : C_UdpSocketRuntimeSpawnFailed;
}

C_UdpSocketResultCode galay_kernel_udp_socket_close(
    galay_kernel_runtime_t* runtime,
    galay_kernel_udp_socket_t* c_socket,
    galay_kernel_udp_close_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        callback == nullptr)
    {
        return C_UdpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UdpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::UdpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_close(socket, callback, ctx));
    return spawned ? C_UdpSocketSuccess : C_UdpSocketRuntimeSpawnFailed;
}
