#ifndef GALAY_KERNEL_CORE_C_CORO_UDP_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_UDP_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_udp_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 UDP adapter。
 *
 * @details 该内部 C 风格桥接复用 TCP bridge 的 C_IOResult/Host/wait hook
 * ABI，只把 C++ UDP awaitable/controller 细节限制在 kernel core 内。
 */

#ifdef __cplusplus
extern "C" {
#endif

GalayCoreCoroIOResult galay_core_coro_udp_recvfrom(void* socket,
                                                   void* scheduler,
                                                   char* buffer,
                                                   size_t length,
                                                   GalayCoreCoroHost* from,
                                                   int64_t timeout_ms,
                                                   void* user_data,
                                                   const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_udp_sendto(void* socket,
                                                 void* scheduler,
                                                 const char* buffer,
                                                 size_t length,
                                                 const GalayCoreCoroHost* to,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_udp_close(void* socket,
                                                void* scheduler,
                                                int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
