#ifndef GALAY_C_KERNEL_KERNEL_C_H
#define GALAY_C_KERNEL_KERNEL_C_H

/**
 * @file kernel_c.h
 * @brief Galay kernel C ABI umbrella header.
 *
 * @details This header is the public aggregate entry for the kernel C ABI.
 *          Headers not included here are treated as internal implementation
 *          details and should not be used by language bindings directly.
 */

#include "common-c/host.h"
#include "core-c/runtime_c.h"
#include "coro-c/coro_result_c.h"
#include "coro-c/coro_task_c.h"
#include "coro-c/coro_wait_c.h"
#include "async-c/aio_file_c.h"
#include "async-c/async_file_c.h"
#include "async-c/file_watcher_c.h"
#include "async-c/tcp_socket_c.h"
#include "async-c/udp_socket_c.h"
#include "concurrency-c/async_mutex_c.h"
#include "concurrency-c/async_waiter_c.h"
#include "concurrency-c/mpsc_channel_c.h"
#include "concurrency-c/unsafe_channel_c.h"

#endif /* GALAY_C_KERNEL_KERNEL_C_H */
