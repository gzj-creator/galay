#include "tcp_socket_c.h"

#include "../../../cpp/galay-kernel/async/tcp_socket.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <netinet/in.h>
#include <new>
#include <string>
#include <sys/socket.h>
#include <utility>

namespace
{

bool is_valid_c_ip_type(C_IPType ip_type) {
    return ip_type == C_IPTypeIPV4 || ip_type == C_IPTypeIPV6;
}

galay::kernel::IPType from_c_ip_type_to_cpp_ip_type(C_IPType ip_type) {
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

C_TcpSocketResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    return galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid)
        ? C_TcpSocketParameterInvalid
        : C_TcpSocketIOFailed;
}

C_TcpSocketResultCode from_cpp_timeout_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kTimeout))
    {
        return C_TcpSocketTimeout;
    }
    return from_cpp_io_error(error);
}

bool timeout_fits_chrono(uint64_t timeout_ms)
{
    using Rep = std::chrono::milliseconds::rep;
    if constexpr (std::numeric_limits<Rep>::is_signed)
    {
        return timeout_ms <= static_cast<uint64_t>(std::numeric_limits<Rep>::max());
    }
    return timeout_ms <= std::numeric_limits<Rep>::max();
}

std::chrono::milliseconds to_timeout(uint64_t timeout_ms)
{
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

galay::kernel::Task<void> c_api_connect(
    galay::async::TcpSocket* socket,
    galay::kernel::Host host,
    galay_kernel_tcp_connect_callback_t callback,
    void* ctx)
{
    auto connected = co_await socket->connect(host);
    callback(connected ? C_TcpSocketSuccess : from_cpp_io_error(connected.error()), ctx);
    co_return;
}

galay::kernel::Task<void> c_api_connect_timeout(
    galay::async::TcpSocket* socket,
    galay::kernel::Host host,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_connect_callback_t callback,
    void* ctx)
{
    auto connected = co_await socket->connect(host).timeout(timeout);
    callback(connected ? C_TcpSocketSuccess : from_cpp_timeout_io_error(connected.error()), ctx);
    co_return;
}

galay::kernel::Task<void> c_api_accept(
    galay::async::TcpSocket* listener,
    galay_kernel_tcp_accept_callback_t callback,
    void* ctx)
{
    galay::kernel::Host peer;
    auto accepted = co_await listener->accept(&peer);

    galay_kernel_tcp_accept_result_t result{};
    if (!accepted)
    {
        result.code = from_cpp_io_error(accepted.error());
        callback(&result, ctx);
        co_return;
    }

    auto accepted_socket = galay::async::TcpSocket(*accepted);
    auto non_block = accepted_socket.option().handleNonBlock();
    if (!non_block || !assign_c_host_from_cpp_host(peer, &result.peer))
    {
        result.code = C_TcpSocketIOFailed;
        callback(&result, ctx);
        co_return;
    }

    auto* cpp_socket = new (std::nothrow) galay::async::TcpSocket(std::move(accepted_socket));
    if (cpp_socket == nullptr)
    {
        result.code = C_TcpSocketMemoryAllocFailed;
        callback(&result, ctx);
        co_return;
    }

    auto c_socket = galay_kernel_tcp_socket_t{};

    c_socket.socket = cpp_socket;
    result.code = C_TcpSocketSuccess;
    result.socket = c_socket;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_accept_timeout(
    galay::async::TcpSocket* listener,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_accept_callback_t callback,
    void* ctx)
{
    galay::kernel::Host peer;
    auto accepted = co_await listener->accept(&peer).timeout(timeout);

    galay_kernel_tcp_accept_result_t result{};
    if (!accepted)
    {
        result.code = from_cpp_timeout_io_error(accepted.error());
        callback(&result, ctx);
        co_return;
    }

    auto accepted_socket = galay::async::TcpSocket(*accepted);
    auto non_block = accepted_socket.option().handleNonBlock();
    if (!non_block || !assign_c_host_from_cpp_host(peer, &result.peer))
    {
        result.code = C_TcpSocketIOFailed;
        callback(&result, ctx);
        co_return;
    }

    auto* cpp_socket = new (std::nothrow) galay::async::TcpSocket(std::move(accepted_socket));
    if (cpp_socket == nullptr)
    {
        result.code = C_TcpSocketMemoryAllocFailed;
        callback(&result, ctx);
        co_return;
    }

    result.code = C_TcpSocketSuccess;
    result.socket.socket = cpp_socket;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_accept_loop(
    galay::async::TcpSocket* listener,
    galay_kernel_tcp_accept_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        galay::kernel::Host peer;
        auto accepted = co_await listener->accept(&peer);

        galay_kernel_tcp_accept_result_t result{};
        if (!accepted)
        {
            result.code = from_cpp_io_error(accepted.error());
            (void)callback(&result, ctx);
            co_return;
        }

        auto accepted_socket = galay::async::TcpSocket(*accepted);
        auto non_block = accepted_socket.option().handleNonBlock();
        if (!non_block || !assign_c_host_from_cpp_host(peer, &result.peer))
        {
            result.code = C_TcpSocketIOFailed;
            (void)callback(&result, ctx);
            co_return;
        }

        auto* cpp_socket = new (std::nothrow) galay::async::TcpSocket(std::move(accepted_socket));
        if (cpp_socket == nullptr)
        {
            result.code = C_TcpSocketMemoryAllocFailed;
            (void)callback(&result, ctx);
            co_return;
        }

        result.code = C_TcpSocketSuccess;
        result.socket.socket = cpp_socket;
        if (callback(&result, ctx) != 0)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_accept_loop_timeout(
    galay::async::TcpSocket* listener,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_accept_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        galay::kernel::Host peer;
        auto accepted = co_await listener->accept(&peer).timeout(timeout);

        galay_kernel_tcp_accept_result_t result{};
        if (!accepted)
        {
            result.code = from_cpp_timeout_io_error(accepted.error());
            (void)callback(&result, ctx);
            co_return;
        }

        auto accepted_socket = galay::async::TcpSocket(*accepted);
        auto non_block = accepted_socket.option().handleNonBlock();
        if (!non_block || !assign_c_host_from_cpp_host(peer, &result.peer))
        {
            result.code = C_TcpSocketIOFailed;
            (void)callback(&result, ctx);
            co_return;
        }

        auto* cpp_socket = new (std::nothrow) galay::async::TcpSocket(std::move(accepted_socket));
        if (cpp_socket == nullptr)
        {
            result.code = C_TcpSocketMemoryAllocFailed;
            (void)callback(&result, ctx);
            co_return;
        }

        result.code = C_TcpSocketSuccess;
        result.socket.socket = cpp_socket;
        if (callback(&result, ctx) != 0)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_recv(
    galay::async::TcpSocket* socket,
    char* buffer,
    size_t length,
    galay_kernel_tcp_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await socket->recv(buffer, length);
    galay_kernel_tcp_recv_result_t result{};
    result.code = received ? C_TcpSocketSuccess : from_cpp_io_error(received.error());
    result.buffer = buffer;
    result.length = length;
    result.bytes = received ? *received : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_timeout(
    galay::async::TcpSocket* socket,
    char* buffer,
    size_t length,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await socket->recv(buffer, length).timeout(timeout);
    galay_kernel_tcp_recv_result_t result{};
    result.code = received ? C_TcpSocketSuccess : from_cpp_timeout_io_error(received.error());
    result.buffer = buffer;
    result.length = length;
    result.bytes = received ? *received : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_loop(
    galay::async::TcpSocket* socket,
    char* buffer,
    size_t length,
    galay_kernel_tcp_recv_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        auto received = co_await socket->recv(buffer, length);
        galay_kernel_tcp_recv_result_t result{};
        result.code = received ? C_TcpSocketSuccess : from_cpp_io_error(received.error());
        result.buffer = buffer;
        result.length = length;
        result.bytes = received ? *received : 0;
        const int should_stop = callback(&result, ctx);
        if (should_stop != 0 || !received || *received == 0)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_recv_loop_timeout(
    galay::async::TcpSocket* socket,
    char* buffer,
    size_t length,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_recv_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        auto received = co_await socket->recv(buffer, length).timeout(timeout);
        galay_kernel_tcp_recv_result_t result{};
        result.code = received ? C_TcpSocketSuccess : from_cpp_timeout_io_error(received.error());
        result.buffer = buffer;
        result.length = length;
        result.bytes = received ? *received : 0;
        const int should_stop = callback(&result, ctx);
        if (should_stop != 0 || !received || *received == 0)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_send(
    galay::async::TcpSocket* socket,
    const char* buffer,
    size_t length,
    galay_kernel_tcp_send_callback_t callback,
    void* ctx)
{
    auto sent = co_await socket->send(buffer, length);
    galay_kernel_tcp_send_result_t result{};
    result.code = sent ? C_TcpSocketSuccess : from_cpp_io_error(sent.error());
    result.buffer = buffer;
    result.length = length;
    result.bytes = sent ? *sent : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_send_timeout(
    galay::async::TcpSocket* socket,
    const char* buffer,
    size_t length,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_send_callback_t callback,
    void* ctx)
{
    auto sent = co_await socket->send(buffer, length).timeout(timeout);
    galay_kernel_tcp_send_result_t result{};
    result.code = sent ? C_TcpSocketSuccess : from_cpp_timeout_io_error(sent.error());
    result.buffer = buffer;
    result.length = length;
    result.bytes = sent ? *sent : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_send_loop(
    galay::async::TcpSocket* socket,
    const char* buffer,
    size_t length,
    galay_kernel_tcp_send_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        auto sent = co_await socket->send(buffer, length);
        galay_kernel_tcp_send_result_t result{};
        result.code = sent ? C_TcpSocketSuccess : from_cpp_io_error(sent.error());
        result.buffer = buffer;
        result.length = length;
        result.bytes = sent ? *sent : 0;
        const int should_stop = callback(&result, ctx);
        if (should_stop != 0 || !sent || *sent == 0)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_send_loop_timeout(
    galay::async::TcpSocket* socket,
    const char* buffer,
    size_t length,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_send_loop_callback_t callback,
    void* ctx)
{
    while (true)
    {
        auto sent = co_await socket->send(buffer, length).timeout(timeout);
        galay_kernel_tcp_send_result_t result{};
        result.code = sent ? C_TcpSocketSuccess : from_cpp_timeout_io_error(sent.error());
        result.buffer = buffer;
        result.length = length;
        result.bytes = sent ? *sent : 0;
        const int should_stop = callback(&result, ctx);
        if (should_stop != 0 || !sent || *sent == 0)
        {
            co_return;
        }
    }
}

galay::kernel::Task<void> c_api_close(
    galay::async::TcpSocket* socket,
    galay_kernel_tcp_close_callback_t callback,
    void* ctx)
{
    auto closed = co_await socket->close();
    callback(closed ? C_TcpSocketSuccess : from_cpp_io_error(closed.error()), ctx);
    co_return;
}

galay::kernel::Task<void> c_api_close_timeout(
    galay::async::TcpSocket* socket,
    std::chrono::milliseconds timeout,
    galay_kernel_tcp_close_callback_t callback,
    void* ctx)
{
    auto closed = co_await socket->close().timeout(timeout);
    callback(closed ? C_TcpSocketSuccess : from_cpp_timeout_io_error(closed.error()), ctx);
    co_return;
}

} // namespace

const char* galay_kernel_tcp_socket_get_error(C_TcpSocketResultCode code)
{
    switch (code)
    {
    case C_TcpSocketSuccess:
        return "success";
    case C_TcpSocketParameterInvalid:
        return "parameter invalid";
    case C_TcpSocketMemoryAllocFailed:
        return "memory allocation failed";
    case C_TcpSocketIOFailed:
        return "io failed";
    case C_TcpSocketOperationInvalid:
        return "operation invalid";
    case C_TcpSocketRuntimeNotRunning:
        return "runtime not running";
    case C_TcpSocketRuntimeSpawnFailed:
        return "runtime spawn failed";
    case C_TcpSocketTimeout:
        return "timeout";
    }
    return "unknown tcp socket error";
}

C_TcpSocketResultCode galay_kernel_tcp_socket_create(galay_kernel_tcp_socket_t* c_socket, C_IPType type)
{
    if (c_socket == nullptr || !is_valid_c_ip_type(type))
    {
        return C_TcpSocketParameterInvalid;
    }

    c_socket->socket = nullptr;
    auto result = galay::async::TcpSocket::create(from_c_ip_type_to_cpp_ip_type(type));
    if (!result)
    {
        return C_TcpSocketMemoryAllocFailed;
    }

    auto socket = new (std::nothrow) galay::async::TcpSocket(std::move(*result));
    if (socket == nullptr)
    {
        return C_TcpSocketMemoryAllocFailed;
    }

    c_socket->socket = socket;
    return C_TcpSocketSuccess;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t* c_socket)
{
    if (c_socket == nullptr) return C_TcpSocketParameterInvalid;
    delete static_cast<galay::async::TcpSocket*>(c_socket->socket);
    c_socket->socket = nullptr;
    return C_TcpSocketSuccess;
}


C_TcpSocketResultCode galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || host == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto reuse_addr = socket->option().handleReuseAddr();
    if (!reuse_addr)
    {
        return C_TcpSocketIOFailed;
    }
    auto non_block = socket->option().handleNonBlock();
    if (!non_block)
    {
        return C_TcpSocketIOFailed;
    }
    auto bound = socket->bind(from_c_host_to_cpp_host(*host));
    if (!bound)
    {
        return galay::kernel::IOError::contains(bound.error().code(), galay::kernel::kParamInvalid)
            ? C_TcpSocketParameterInvalid
            : C_TcpSocketIOFailed;
    }
    return C_TcpSocketSuccess;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_listen(galay_kernel_tcp_socket_t* c_socket, int backlog)
{
    if (c_socket == nullptr || c_socket->socket == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto listened = socket->listen(backlog);
    return listened ? C_TcpSocketSuccess : C_TcpSocketIOFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* c_socket,
    C_Host* endpoint)
{
    if (c_socket == nullptr || c_socket->socket == nullptr || endpoint == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    const auto* socket = static_cast<const galay::async::TcpSocket*>(c_socket->socket);
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(socket->handle().fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
    {
        return C_TcpSocketIOFailed;
    }

    return assign_c_host_from_sockaddr(storage, endpoint) ? C_TcpSocketSuccess : C_TcpSocketIOFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_connect(
    galay_kernel_runtime_t* c_runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host,
    galay_kernel_tcp_connect_callback_t callback,
    void* ctx)
{
    if (c_runtime == nullptr || c_runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        host == nullptr || callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto cpp_host = from_c_host_to_cpp_host(*host);
    if (!cpp_host.valid())
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* runtime = to_cpp_runtime(c_runtime);
    if (!runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto non_block = socket->option().handleNonBlock();
    if (!non_block)
    {
        return C_TcpSocketIOFailed;
    }

    auto spawned = runtime->spawn(c_api_connect(socket, cpp_host, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_connect_timeout(
    galay_kernel_runtime_t* c_runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const C_Host* host,
    uint64_t timeout_ms,
    galay_kernel_tcp_connect_callback_t callback,
    void* ctx)
{
    if (c_runtime == nullptr || c_runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        host == nullptr || callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto cpp_host = from_c_host_to_cpp_host(*host);
    if (!cpp_host.valid())
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* runtime = to_cpp_runtime(c_runtime);
    if (!runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto non_block = socket->option().handleNonBlock();
    if (!non_block)
    {
        return C_TcpSocketIOFailed;
    }

    auto spawned = runtime->spawn(
        c_api_connect_timeout(socket, cpp_host, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_accept(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        listener == nullptr || listener->socket == nullptr ||
        callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(listener->socket);
    auto spawned = cpp_runtime->spawn(c_api_accept(socket, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_accept_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    uint64_t timeout_ms,
    galay_kernel_tcp_accept_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        listener == nullptr || listener->socket == nullptr ||
        callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(listener->socket);
    auto spawned = cpp_runtime->spawn(c_api_accept_timeout(socket, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_accept_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        listener == nullptr || listener->socket == nullptr ||
        callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(listener->socket);
    auto spawned = cpp_runtime->spawn(c_api_accept_loop(socket, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_accept_loop_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    uint64_t timeout_ms,
    galay_kernel_tcp_accept_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        listener == nullptr || listener->socket == nullptr ||
        callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(listener->socket);
    auto spawned = cpp_runtime->spawn(
        c_api_accept_loop_timeout(socket, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_recv(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_tcp_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_recv(socket, buffer, length, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_recv_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    char* buffer,
    size_t length,
    uint64_t timeout_ms,
    galay_kernel_tcp_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr ||
        !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(
        c_api_recv_timeout(socket, buffer, length, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_recv_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    char* buffer,
    size_t length,
    galay_kernel_tcp_recv_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_recv_loop(socket, buffer, length, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_recv_loop_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    char* buffer,
    size_t length,
    uint64_t timeout_ms,
    galay_kernel_tcp_recv_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr ||
        !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(
        c_api_recv_loop_timeout(socket, buffer, length, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_send(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    galay_kernel_tcp_send_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_send(socket, buffer, length, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_send_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    uint64_t timeout_ms,
    galay_kernel_tcp_send_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr ||
        !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(
        c_api_send_timeout(socket, buffer, length, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_send_loop(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    galay_kernel_tcp_send_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_send_loop(socket, buffer, length, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_send_loop_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    const char* buffer,
    size_t length,
    uint64_t timeout_ms,
    galay_kernel_tcp_send_loop_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        buffer == nullptr || length == 0 || callback == nullptr ||
        !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(
        c_api_send_loop_timeout(socket, buffer, length, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_close(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    galay_kernel_tcp_close_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        callback == nullptr)
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_close(socket, callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}

C_TcpSocketResultCode galay_kernel_tcp_socket_close_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* c_socket,
    uint64_t timeout_ms,
    galay_kernel_tcp_close_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_socket == nullptr || c_socket->socket == nullptr ||
        callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_TcpSocketParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_TcpSocketRuntimeNotRunning;
    }

    auto* socket = static_cast<galay::async::TcpSocket*>(c_socket->socket);
    auto spawned = cpp_runtime->spawn(c_api_close_timeout(socket, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_TcpSocketSuccess : C_TcpSocketRuntimeSpawnFailed;
}
