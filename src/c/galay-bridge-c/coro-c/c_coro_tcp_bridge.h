#ifndef GALAY_KERNEL_CORE_C_CORO_TCP_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_TCP_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

/**
 * @file c_coro_tcp_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 TCP adapter。
 *
 * @details 这是 C ABI 层使用的内部 C 风格桥接。它将 C++ awaitable/controller
 * 细节限制在 kernel core 内，同时允许 C coroutine runtime 提供 wait 和 completion hook。
 */

#define GALAY_CORE_CORO_HOST_ADDRESS_MAX_LENGTH 46

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GalayCoreCoroIOResultCode {
    GalayCoreCoroIOResultOk,
    GalayCoreCoroIOResultEof,
    GalayCoreCoroIOResultTimeout,
    GalayCoreCoroIOResultCancelled,
    GalayCoreCoroIOResultInvalid,
    GalayCoreCoroIOResultError,
} GalayCoreCoroIOResultCode;

typedef struct GalayCoreCoroIOResult {
    GalayCoreCoroIOResultCode code;
    int sys_errno;
    size_t bytes;
    int64_t value;
    void* ptr;
} GalayCoreCoroIOResult;

typedef enum GalayCoreCoroIPType {
    GalayCoreCoroIPTypeIPV4 = 0,
    GalayCoreCoroIPTypeIPV6 = 1,
} GalayCoreCoroIPType;

typedef struct GalayCoreCoroHost {
    GalayCoreCoroIPType type;
    char address[GALAY_CORE_CORO_HOST_ADDRESS_MAX_LENGTH];
    uint16_t port;
} GalayCoreCoroHost;

typedef GalayCoreCoroIOResult (*GalayCoreCoroWaitFn)(void* ctx, int64_t timeout_ms);
typedef GalayCoreCoroIOResult (*GalayCoreCoroCompleteUserDataFn)(
    void* user_data,
    GalayCoreCoroIOResult result);
typedef GalayCoreCoroIOResult (*GalayCoreCoroReleaseUserDataFn)(void* user_data);

typedef struct GalayCoreCoroWaitOps {
    GalayCoreCoroWaitFn wait;
    GalayCoreCoroCompleteUserDataFn complete_user_data;
    GalayCoreCoroReleaseUserDataFn release_user_data;
    void* ctx;
} GalayCoreCoroWaitOps;

GalayCoreCoroIOResult galay_core_coro_tcp_accept(void* listener_socket,
                                                 void* scheduler,
                                                 void** out_socket,
                                                 GalayCoreCoroHost* out_peer,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_connect(void* socket,
                                                  void* scheduler,
                                                  const GalayCoreCoroHost* host,
                                                  int64_t timeout_ms,
                                                  void* user_data,
                                                  const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_recv(void* socket,
                                               void* scheduler,
                                               char* buffer,
                                               size_t length,
                                               int64_t timeout_ms,
                                               void* user_data,
                                               const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_send(void* socket,
                                               void* scheduler,
                                               const char* buffer,
                                               size_t length,
                                               int64_t timeout_ms,
                                               void* user_data,
                                               const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_readv(void* socket,
                                                void* scheduler,
                                                const struct iovec* iovecs,
                                                size_t count,
                                                int64_t timeout_ms,
                                                void* user_data,
                                                const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_writev(void* socket,
                                                 void* scheduler,
                                                 const struct iovec* iovecs,
                                                 size_t count,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_sendfile(void* socket,
                                                   void* scheduler,
                                                   int file_fd,
                                                   int64_t offset,
                                                   size_t count,
                                                   int64_t timeout_ms,
                                                   void* user_data,
                                                   const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_tcp_close(void* socket,
                                                void* scheduler,
                                                int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
