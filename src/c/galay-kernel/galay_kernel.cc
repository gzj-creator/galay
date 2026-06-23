#include <galay/c/galay-kernel/galay_kernel.h>

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/async/udp_socket.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <arpa/inet.h>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

struct galay_kernel_runtime {
    galay::kernel::Runtime runtime;
};

struct galay_kernel_tcp_socket {
    galay::async::TcpSocket socket;
    mutable std::array<char, INET6_ADDRSTRLEN> endpoint_address{};
};

struct galay_kernel_udp_socket {
    galay::async::UdpSocket socket;
};

struct AcceptResult {
    galay_status_t status = GALAY_INTERNAL_ERROR;
    std::optional<GHandle> accepted_handle;
    galay_kernel_ip_type_t peer_ip_type = GALAY_KERNEL_IP_V4;
    std::array<char, INET6_ADDRSTRLEN> peer_address{};
    uint16_t peer_port = 0;
};

struct galay_kernel_tcp_accept {
    std::optional<galay::kernel::JoinHandle<AcceptResult>> handle;
    bool joined = false;
    std::array<char, INET6_ADDRSTRLEN> peer_address{};
};

namespace
{

bool is_valid_ip_type(galay_kernel_ip_type_t ip_type) noexcept
{
    return ip_type == GALAY_KERNEL_IP_V4 || ip_type == GALAY_KERNEL_IP_V6;
}

galay::kernel::IPType to_cpp_ip_type(galay_kernel_ip_type_t ip_type) noexcept
{
    return ip_type == GALAY_KERNEL_IP_V6
        ? galay::kernel::IPType::IPV6
        : galay::kernel::IPType::IPV4;
}

galay::kernel::RuntimeConfig to_cpp_runtime_config(const galay_kernel_runtime_config_t& config)
{
    galay::kernel::RuntimeConfig cpp_config;
    cpp_config.io_scheduler_count = config.io_scheduler_count;
    cpp_config.compute_scheduler_count = config.compute_scheduler_count;
    return cpp_config;
}

galay_status_t validate_host_config(galay_kernel_ip_type_t ip_type, const char* address) noexcept
{
    if (!is_valid_ip_type(ip_type) || address == nullptr || address[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }

    unsigned char buffer[sizeof(in6_addr)]{};
    const int family = ip_type == GALAY_KERNEL_IP_V4 ? AF_INET : AF_INET6;
    return inet_pton(family, address, buffer) == 1 ? GALAY_OK : GALAY_INVALID_ARGUMENT;
}

galay::kernel::Host to_cpp_host(const galay_kernel_tcp_host_config_t& config) noexcept
{
    galay::kernel::Host host;
    std::memset(&host.m_addr, 0, sizeof(host.m_addr));
    if (config.ip_type == GALAY_KERNEL_IP_V6) {
        auto* addr = reinterpret_cast<sockaddr_in6*>(&host.m_addr);
        addr->sin6_family = AF_INET6;
        addr->sin6_port = htons(config.port);
        (void)::inet_pton(AF_INET6, config.address, &addr->sin6_addr);
        host.m_addr_len = sizeof(sockaddr_in6);
        return host;
    }

    auto* addr = reinterpret_cast<sockaddr_in*>(&host.m_addr);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(config.port);
    (void)::inet_pton(AF_INET, config.address, &addr->sin_addr);
    host.m_addr_len = sizeof(sockaddr_in);
    return host;
}

galay_status_t assign_endpoint_config(
    const sockaddr_storage& storage,
    char* address_storage,
    size_t address_storage_size,
    galay_kernel_tcp_host_config_t* out_host)
{
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, address_storage, address_storage_size) == nullptr) {
            return GALAY_IO_ERROR;
        }
        *out_host = galay_kernel_tcp_host_config_t{
            GALAY_KERNEL_IP_V4,
            address_storage,
            ntohs(addr->sin_port)
        };
        return GALAY_OK;
    }

    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, address_storage, address_storage_size) == nullptr) {
            return GALAY_IO_ERROR;
        }
        *out_host = galay_kernel_tcp_host_config_t{
            GALAY_KERNEL_IP_V6,
            address_storage,
            ntohs(addr->sin6_port)
        };
        return GALAY_OK;
    }

    return GALAY_INTERNAL_ERROR;
}

galay_status_t assign_accept_peer(const galay::kernel::Host& peer, AcceptResult& result) noexcept
{
    const auto& storage = peer.m_addr;
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, result.peer_address.data(), result.peer_address.size()) == nullptr) {
            return GALAY_IO_ERROR;
        }
        result.peer_ip_type = GALAY_KERNEL_IP_V4;
        result.peer_port = ntohs(addr->sin_port);
        return GALAY_OK;
    }

    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, result.peer_address.data(), result.peer_address.size()) == nullptr) {
            return GALAY_IO_ERROR;
        }
        result.peer_ip_type = GALAY_KERNEL_IP_V6;
        result.peer_port = ntohs(addr->sin6_port);
        return GALAY_OK;
    }

    return GALAY_INTERNAL_ERROR;
}

void close_if_valid(GHandle handle) noexcept
{
    if (!(handle == GHandle::invalid())) {
        (void)::close(handle.fd);
    }
}

template <typename Fn>
galay_status_t catch_kernel_boundary(Fn&& fn) noexcept
{
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (const std::exception&) {
        return GALAY_INTERNAL_ERROR;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

template <typename Fn>
galay_bool_t catch_kernel_bool_boundary(Fn&& fn) noexcept
{
    try {
        return fn();
    } catch (...) {
        return GALAY_FALSE;
    }
}

galay::kernel::Task<AcceptResult> c_tcp_accept(galay::async::TcpSocket* listener)
{
    if (listener == nullptr) {
        AcceptResult result;
        result.status = GALAY_INVALID_ARGUMENT;
        co_return result;
    }

    galay::kernel::Host peer;
    auto accepted = co_await listener->accept(&peer);
    if (!accepted) {
        AcceptResult result;
        result.status = GALAY_IO_ERROR;
        co_return result;
    }

    AcceptResult result;
    result.status = GALAY_OK;
    result.accepted_handle = accepted.value();
    result.status = assign_accept_peer(peer, result);
    co_return result;
}

} // namespace

extern "C" {

galay_kernel_runtime_config_t galay_kernel_runtime_config_default(void)
{
    return galay_kernel_runtime_config_t{
        GALAY_KERNEL_SCHEDULER_COUNT_AUTO,
        GALAY_KERNEL_SCHEDULER_COUNT_AUTO
    };
}

galay_status_t galay_kernel_runtime_create(
    const galay_kernel_runtime_config_t* config,
    galay_kernel_runtime_t** out_runtime)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (config == nullptr || out_runtime == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        *out_runtime = nullptr;
        auto* runtime = new (std::nothrow) galay_kernel_runtime{
            galay::kernel::Runtime(to_cpp_runtime_config(*config))
        };
        if (runtime == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_runtime = runtime;
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_runtime_start(galay_kernel_runtime_t* runtime)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (runtime == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        auto started = runtime->runtime.start();
        return started ? GALAY_OK : GALAY_INTERNAL_ERROR;
    });
}

galay_status_t galay_kernel_runtime_stop(galay_kernel_runtime_t* runtime)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (runtime == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        runtime->runtime.stop();
        return GALAY_OK;
    });
}

galay_bool_t galay_kernel_runtime_is_running(const galay_kernel_runtime_t* runtime)
{
    return catch_kernel_bool_boundary([&]() -> galay_bool_t {
        if (runtime == nullptr) {
            return GALAY_FALSE;
        }
        return runtime->runtime.isRunning() ? GALAY_TRUE : GALAY_FALSE;
    });
}

galay_status_t galay_kernel_runtime_destroy(galay_kernel_runtime_t** runtime)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (runtime == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (*runtime == nullptr) {
            return GALAY_OK;
        }

        delete *runtime;
        *runtime = nullptr;
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_tcp_host_config_validate(
    const galay_kernel_tcp_host_config_t* config)
{
    if (config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return validate_host_config(config->ip_type, config->address);
}

galay_status_t galay_kernel_udp_host_config_validate(
    const galay_kernel_udp_host_config_t* config)
{
    if (config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return validate_host_config(config->ip_type, config->address);
}

galay_status_t galay_kernel_tcp_socket_create(
    galay_kernel_ip_type_t ip_type,
    galay_kernel_tcp_socket_t** out_socket)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (!is_valid_ip_type(ip_type) || out_socket == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        *out_socket = nullptr;
        auto created = galay::async::TcpSocket::create(to_cpp_ip_type(ip_type));
        if (!created) {
            return GALAY_IO_ERROR;
        }

        auto* socket = new (std::nothrow) galay_kernel_tcp_socket{
            std::move(*created),
            {}
        };
        if (socket == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_socket = socket;
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t** socket)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (socket == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (*socket == nullptr) {
            return GALAY_OK;
        }

        delete *socket;
        *socket = nullptr;
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* socket,
    const galay_kernel_tcp_host_config_t* host)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (socket == nullptr || host == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (validate_host_config(host->ip_type, host->address) != GALAY_OK) {
            return GALAY_INVALID_ARGUMENT;
        }

        auto reuse_addr = socket->socket.option().handleReuseAddr();
        if (!reuse_addr) {
            return GALAY_IO_ERROR;
        }
        auto non_block = socket->socket.option().handleNonBlock();
        if (!non_block) {
            return GALAY_IO_ERROR;
        }
        auto bound = socket->socket.bind(to_cpp_host(*host));
        if (!bound) {
            return GALAY_IO_ERROR;
        }
        socket->endpoint_address[0] = '\0';
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_tcp_socket_listen(galay_kernel_tcp_socket_t* socket, int backlog)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (socket == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        auto result = socket->socket.listen(backlog);
        return result ? GALAY_OK : GALAY_IO_ERROR;
    });
}

galay_status_t galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* socket,
    galay_kernel_tcp_host_config_t* out_host)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (socket == nullptr || out_host == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        sockaddr_storage storage{};
        socklen_t length = sizeof(storage);
        if (::getsockname(socket->socket.handle().fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            return GALAY_IO_ERROR;
        }

        return assign_endpoint_config(
            storage,
            socket->endpoint_address.data(),
            socket->endpoint_address.size(),
            out_host);
    });
}

galay_status_t galay_kernel_tcp_accept_start(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_t** out_accept)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (runtime == nullptr || listener == nullptr || out_accept == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        *out_accept = nullptr;
        std::unique_ptr<galay_kernel_tcp_accept> accept(new (std::nothrow) galay_kernel_tcp_accept{});
        if (accept == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }

        auto started = runtime->runtime.start();
        if (!started) {
            return GALAY_INTERNAL_ERROR;
        }
        auto* scheduler = runtime->runtime.getNextIOScheduler();
        if (scheduler == nullptr) {
            return GALAY_INTERNAL_ERROR;
        }

        auto task = c_tcp_accept(&listener->socket);
        auto task_ref = galay::kernel::detail::TaskAccess::detachTask(std::move(task));
        galay::kernel::detail::setTaskRuntime(task_ref, &runtime->runtime);
        galay::kernel::detail::setTaskScheduler(task_ref, scheduler);
        if (!galay::kernel::detail::scheduleTask(task_ref)) {
            return GALAY_INTERNAL_ERROR;
        }

        accept->handle.emplace(galay::kernel::JoinHandle<AcceptResult>(std::move(task_ref)));
        *out_accept = accept.release();
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_tcp_accept_wait(galay_kernel_tcp_accept_t* accept)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (accept == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (accept->joined) {
            return GALAY_OK;
        }
        if (!accept->handle.has_value()) {
            return GALAY_INVALID_ARGUMENT;
        }

        auto result = accept->handle->wait();
        return result ? GALAY_OK : GALAY_INTERNAL_ERROR;
    });
}

galay_status_t galay_kernel_tcp_accept_join(
    galay_kernel_tcp_accept_t* accept,
    galay_kernel_tcp_socket_t** out_socket,
    galay_kernel_tcp_host_config_t* out_peer)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (out_socket != nullptr) {
            *out_socket = nullptr;
        }
        if (accept == nullptr || out_socket == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (accept->joined || !accept->handle.has_value()) {
            return GALAY_INVALID_ARGUMENT;
        }

        auto joined = accept->handle->join();
        accept->joined = true;
        accept->handle.reset();
        if (!joined) {
            return GALAY_INTERNAL_ERROR;
        }
        if (joined->status != GALAY_OK) {
            return joined->status;
        }
        if (!joined->accepted_handle.has_value()) {
            return GALAY_INTERNAL_ERROR;
        }

        GHandle accepted_handle = joined->accepted_handle.value();
        auto* socket = new (std::nothrow) galay_kernel_tcp_socket{
            galay::async::TcpSocket(accepted_handle),
            {}
        };
        if (socket == nullptr) {
            close_if_valid(accepted_handle);
            return GALAY_OUT_OF_MEMORY;
        }

        if (out_peer != nullptr) {
            accept->peer_address = joined->peer_address;
            *out_peer = galay_kernel_tcp_host_config_t{
                joined->peer_ip_type,
                accept->peer_address.data(),
                joined->peer_port
            };
        }

        *out_socket = socket;
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_tcp_accept_destroy(galay_kernel_tcp_accept_t** accept)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (accept == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (*accept == nullptr) {
            return GALAY_OK;
        }

        galay_status_t status = GALAY_OK;
        if (!(*accept)->joined && (*accept)->handle.has_value()) {
            auto joined = (*accept)->handle->join();
            (*accept)->joined = true;
            (*accept)->handle.reset();
            if (joined) {
                if (joined->accepted_handle.has_value()) {
                    close_if_valid(joined->accepted_handle.value());
                }
            } else {
                status = GALAY_INTERNAL_ERROR;
            }
        }
        delete *accept;
        *accept = nullptr;
        return status;
    });
}

galay_status_t galay_kernel_udp_socket_create(
    galay_kernel_ip_type_t ip_type,
    galay_kernel_udp_socket_t** out_socket)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (!is_valid_ip_type(ip_type) || out_socket == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }

        *out_socket = nullptr;
        auto created = galay::async::UdpSocket::create(to_cpp_ip_type(ip_type));
        if (!created) {
            return GALAY_IO_ERROR;
        }

        auto* socket = new (std::nothrow) galay_kernel_udp_socket{
            std::move(*created)
        };
        if (socket == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_socket = socket;
        return GALAY_OK;
    });
}

galay_status_t galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t** socket)
{
    return catch_kernel_boundary([&]() -> galay_status_t {
        if (socket == nullptr) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (*socket == nullptr) {
            return GALAY_OK;
        }

        delete *socket;
        *socket = nullptr;
        return GALAY_OK;
    });
}

} // extern "C"
