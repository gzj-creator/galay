#include <galay/c/galay-kernel/galay_kernel.h>

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/async/udp_socket.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <arpa/inet.h>
#include <new>
#include <unistd.h>

struct galay_kernel_runtime {
    galay::kernel::Runtime runtime;
};

struct galay_kernel_tcp_socket {
    galay::async::TcpSocket socket;
};

struct galay_kernel_udp_socket {
    galay::async::UdpSocket socket;
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

void close_if_valid(GHandle handle) noexcept
{
    if (!(handle == GHandle::invalid())) {
        (void)::close(handle.fd);
    }
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
    if (config == nullptr || out_runtime == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    *out_runtime = nullptr;
    try {
        auto* runtime = new (std::nothrow) galay_kernel_runtime{
            galay::kernel::Runtime(to_cpp_runtime_config(*config))
        };
        if (runtime == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_runtime = runtime;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_kernel_runtime_start(galay_kernel_runtime_t* runtime)
{
    if (runtime == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        runtime->runtime.start();
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_kernel_runtime_stop(galay_kernel_runtime_t* runtime)
{
    if (runtime == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        runtime->runtime.stop();
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_bool_t galay_kernel_runtime_is_running(const galay_kernel_runtime_t* runtime)
{
    if (runtime == nullptr) {
        return GALAY_FALSE;
    }
    return runtime->runtime.isRunning() ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_kernel_runtime_destroy(galay_kernel_runtime_t** runtime)
{
    if (runtime == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (*runtime == nullptr) {
        return GALAY_OK;
    }

    try {
        delete *runtime;
        *runtime = nullptr;
        return GALAY_OK;
    } catch (...) {
        *runtime = nullptr;
        return GALAY_INTERNAL_ERROR;
    }
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
    if (!is_valid_ip_type(ip_type) || out_socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    *out_socket = nullptr;
    try {
        auto* socket = new (std::nothrow) galay_kernel_tcp_socket{
            galay::async::TcpSocket(to_cpp_ip_type(ip_type))
        };
        if (socket == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_socket = socket;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t** socket)
{
    if (socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (*socket == nullptr) {
        return GALAY_OK;
    }

    close_if_valid((*socket)->socket.handle());
    delete *socket;
    *socket = nullptr;
    return GALAY_OK;
}

galay_status_t galay_kernel_udp_socket_create(
    galay_kernel_ip_type_t ip_type,
    galay_kernel_udp_socket_t** out_socket)
{
    if (!is_valid_ip_type(ip_type) || out_socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    *out_socket = nullptr;
    try {
        auto* socket = new (std::nothrow) galay_kernel_udp_socket{
            galay::async::UdpSocket(to_cpp_ip_type(ip_type))
        };
        if (socket == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        *out_socket = socket;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t** socket)
{
    if (socket == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (*socket == nullptr) {
        return GALAY_OK;
    }

    close_if_valid((*socket)->socket.handle());
    delete *socket;
    *socket = nullptr;
    return GALAY_OK;
}

} // extern "C"
