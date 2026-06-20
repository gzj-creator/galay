/**
 * @file galay_kernel.h
 * @brief Galay kernel 模块的 C ABI。
 */

#ifndef GALAY_C_KERNEL_GALAY_KERNEL_H
#define GALAY_C_KERNEL_GALAY_KERNEL_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

#define GALAY_KERNEL_SCHEDULER_COUNT_AUTO ((size_t)-1)

typedef struct galay_kernel_runtime galay_kernel_runtime_t;
typedef struct galay_kernel_tcp_socket galay_kernel_tcp_socket_t;
typedef struct galay_kernel_udp_socket galay_kernel_udp_socket_t;

typedef enum galay_kernel_ip_type {
    GALAY_KERNEL_IP_V4 = 0,
    GALAY_KERNEL_IP_V6 = 1
} galay_kernel_ip_type_t;

typedef struct galay_kernel_runtime_config {
    size_t io_scheduler_count;
    size_t compute_scheduler_count;
} galay_kernel_runtime_config_t;

typedef struct galay_kernel_tcp_host_config {
    galay_kernel_ip_type_t ip_type;
    const char* address;
    uint16_t port;
} galay_kernel_tcp_host_config_t;

typedef struct galay_kernel_udp_host_config {
    galay_kernel_ip_type_t ip_type;
    const char* address;
    uint16_t port;
} galay_kernel_udp_host_config_t;

GALAY_C_API galay_kernel_runtime_config_t galay_kernel_runtime_config_default(void);
GALAY_C_API galay_status_t galay_kernel_runtime_create(
    const galay_kernel_runtime_config_t* config,
    galay_kernel_runtime_t** out_runtime);
GALAY_C_API galay_status_t galay_kernel_runtime_start(galay_kernel_runtime_t* runtime);
GALAY_C_API galay_status_t galay_kernel_runtime_stop(galay_kernel_runtime_t* runtime);
GALAY_C_API galay_bool_t galay_kernel_runtime_is_running(const galay_kernel_runtime_t* runtime);
GALAY_C_API galay_status_t galay_kernel_runtime_destroy(galay_kernel_runtime_t** runtime);

GALAY_C_API galay_status_t galay_kernel_tcp_host_config_validate(
    const galay_kernel_tcp_host_config_t* config);
GALAY_C_API galay_status_t galay_kernel_udp_host_config_validate(
    const galay_kernel_udp_host_config_t* config);

GALAY_C_API galay_status_t galay_kernel_tcp_socket_create(
    galay_kernel_ip_type_t ip_type,
    galay_kernel_tcp_socket_t** out_socket);
GALAY_C_API galay_status_t galay_kernel_tcp_socket_destroy(galay_kernel_tcp_socket_t** socket);
GALAY_C_API galay_status_t galay_kernel_udp_socket_create(
    galay_kernel_ip_type_t ip_type,
    galay_kernel_udp_socket_t** out_socket);
GALAY_C_API galay_status_t galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t** socket);

GALAY_C_END_DECLS

#endif /* GALAY_C_KERNEL_GALAY_KERNEL_H */
