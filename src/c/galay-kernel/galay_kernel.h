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
typedef struct galay_kernel_tcp_accept galay_kernel_tcp_accept_t;
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

/**
 * @brief 绑定 TCP socket 到本地地址。
 * @param socket 由 galay_kernel_tcp_socket_create 创建的 socket。
 * @param host 本地地址配置；address 必须是合法 IPv4/IPv6 字符串。
 * @return 成功返回 GALAY_OK；参数无效返回 GALAY_INVALID_ARGUMENT；系统调用失败返回 GALAY_IO_ERROR。
 * @note 绑定前会设置 SO_REUSEADDR 和非阻塞模式。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_socket_bind(
    galay_kernel_tcp_socket_t* socket,
    const galay_kernel_tcp_host_config_t* host);

/**
 * @brief 让 TCP socket 进入 listen 状态。
 * @param socket 已 bind 的 TCP socket。
 * @param backlog 等待连接队列长度，直接传给底层 listen。
 * @return 成功返回 GALAY_OK；参数无效返回 GALAY_INVALID_ARGUMENT；系统调用失败返回 GALAY_IO_ERROR。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_socket_listen(
    galay_kernel_tcp_socket_t* socket,
    int backlog);

/**
 * @brief 查询 TCP socket 当前本地端点。
 * @param socket TCP socket；函数会刷新其内部地址字符串缓存。
 * @param out_host 输出本地地址；address 指针借用自 socket，直到 socket 被修改或销毁。
 * @return 成功返回 GALAY_OK；参数无效返回 GALAY_INVALID_ARGUMENT；系统调用失败返回 GALAY_IO_ERROR。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_socket_local_endpoint(
    const galay_kernel_tcp_socket_t* socket,
    galay_kernel_tcp_host_config_t* out_host);

/**
 * @brief 在 runtime 上启动一次 TCP accept 协程。
 * @param runtime 已创建的 runtime；必须存活到 accept handle 被销毁之后。
 * @param listener 已 bind/listen 的 TCP 监听 socket；必须存活到 accept handle 被销毁之后。
 * @param out_accept 输出一次性 accept handle；调用方通过 destroy 释放。
 * @return 成功返回 GALAY_OK；参数无效或提交失败返回对应状态码。
 * @note 该 C API 不暴露 C++ coroutine 或 JoinHandle 类型。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_accept_start(
    galay_kernel_runtime_t* runtime,
    galay_kernel_tcp_socket_t* listener,
    galay_kernel_tcp_accept_t** out_accept);

/**
 * @brief 阻塞等待 TCP accept 协程完成但不消费结果。
 * @param accept 由 galay_kernel_tcp_accept_start 返回的 handle。
 * @return 等待成功返回 GALAY_OK；参数或内部状态无效返回错误状态。
 * @note 会阻塞调用线程；不会恢复 C++ coroutine frame，恢复由 Galay IO scheduler 负责。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_accept_wait(galay_kernel_tcp_accept_t* accept);

/**
 * @brief 阻塞等待并消费 TCP accept 结果。
 * @param accept 由 galay_kernel_tcp_accept_start 返回的 handle。
 * @param out_socket 输出 accepted TCP socket；调用方通过 galay_kernel_tcp_socket_destroy 释放。
 * @param out_peer 可选输出对端地址；address 指针借用自 accept handle，直到 handle 销毁。
 * @return 成功返回 GALAY_OK；accept 失败、参数无效或重复 join 返回错误状态。
 * @note join 是一次性操作；会阻塞调用线程。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_accept_join(
    galay_kernel_tcp_accept_t* accept,
    galay_kernel_tcp_socket_t** out_socket,
    galay_kernel_tcp_host_config_t* out_peer);

/**
 * @brief 销毁 TCP accept handle。
 * @param accept accept handle 指针地址；为空 handle 时返回 GALAY_OK。
 * @return 参数无效返回 GALAY_INVALID_ARGUMENT，否则返回 GALAY_OK 或内部错误。
 * @note 若内部 join handle 尚未完成，销毁可能为了释放任务所有权而阻塞等待；不承诺取消。
 */
GALAY_C_API galay_status_t galay_kernel_tcp_accept_destroy(galay_kernel_tcp_accept_t** accept);
GALAY_C_API galay_status_t galay_kernel_udp_socket_create(
    galay_kernel_ip_type_t ip_type,
    galay_kernel_udp_socket_t** out_socket);
GALAY_C_API galay_status_t galay_kernel_udp_socket_destroy(galay_kernel_udp_socket_t** socket);

GALAY_C_END_DECLS

#endif /* GALAY_C_KERNEL_GALAY_KERNEL_H */
