#ifndef GALAY_C_KERNEL_ASYNC_ASYNC_WAITER_H
#define GALAY_C_KERNEL_ASYNC_ASYNC_WAITER_H

#include "../coro-c/coro_result.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum C_AsyncWaiterResultCode {
    C_AsyncWaiterSuccess,
    C_AsyncWaiterParameterInvalid,
    C_AsyncWaiterMemoryAllocFailed,
    C_AsyncWaiterOperationInvalid,
} C_AsyncWaiterResultCode;

typedef struct galay_c_async_waiter {
    void* waiter;
} galay_c_async_waiter_t;

const char* galay_c_async_waiter_get_error(C_AsyncWaiterResultCode code);
C_AsyncWaiterResultCode galay_c_async_waiter_create(galay_c_async_waiter_t* waiter);
C_AsyncWaiterResultCode galay_c_async_waiter_destroy(galay_c_async_waiter_t* waiter);
bool galay_c_async_waiter_is_waiting(const galay_c_async_waiter_t* waiter);
bool galay_c_async_waiter_is_ready(const galay_c_async_waiter_t* waiter);
C_AsyncWaiterResultCode galay_c_async_waiter_notify(galay_c_async_waiter_t* waiter);

/**
 * @brief Waits for the one-shot notification from the current C coroutine.
 * @param waiter Initialized waiter handle.
 * @param timeout_ms -1 waits indefinitely, 0 polls, and a positive value sets a deadline.
 * @return An explicit coroutine result. Only one coroutine may wait at a time.
 */
C_IOResult galay_c_async_waiter_wait(galay_c_async_waiter_t* waiter,
                                          int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
