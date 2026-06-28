#ifndef GALAY_KERNEL_CORE_C_CORO_TCP_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_TCP_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file c_coro_tcp_bridge.h
 * @brief C coroutine TCP adapters exposed by the C++ kernel core.
 *
 * @details This is an internal C-style bridge used by the C ABI layer. It keeps
 * C++ awaitable/controller details inside the kernel core while letting the C
 * coroutine runtime provide wait and completion hooks.
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

GalayCoreCoroIOResult galay_core_coro_tcp_close(void* socket,
                                                void* scheduler,
                                                int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
