#ifndef GALAY_KERNEL_CORE_C_CORO_ASYNC_FILE_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_ASYNC_FILE_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_async_file_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AsyncFile adapter。
 */

#ifdef __cplusplus
extern "C" {
#endif

GalayCoreCoroIOResult galay_core_coro_async_file_read(void* file,
                                                      void* scheduler,
                                                      char* buffer,
                                                      size_t length,
                                                      int64_t offset,
                                                      int64_t timeout_ms,
                                                      void* user_data,
                                                      const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_async_file_write(void* file,
                                                       void* scheduler,
                                                       const char* buffer,
                                                       size_t length,
                                                       int64_t offset,
                                                       int64_t timeout_ms,
                                                       void* user_data,
                                                       const GalayCoreCoroWaitOps* wait_ops);

GalayCoreCoroIOResult galay_core_coro_async_file_close(void* file,
                                                       void* scheduler,
                                                       int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
