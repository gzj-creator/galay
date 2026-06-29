#ifndef GALAY_KERNEL_CORE_C_CORO_AIO_FILE_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_AIO_FILE_BRIDGE_H

#include "c_coro_tcp_bridge.h"

#include <sys/types.h>

/**
 * @file c_coro_aio_file_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AioFile commit adapter。
 */

#ifdef __cplusplus
extern "C" {
#endif

GalayCoreCoroIOResult galay_core_coro_aio_file_commit(void* file,
                                                      void* scheduler,
                                                      ssize_t* results,
                                                      size_t result_capacity,
                                                      size_t* out_count,
                                                      int64_t timeout_ms,
                                                      void* user_data,
                                                      const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
