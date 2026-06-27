#ifndef GALAY_KERNEL_CORO_RESULT_C_H
#define GALAY_KERNEL_CORO_RESULT_C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C coroutine and I/O result code.
 */
typedef enum C_IOResultCode {
    C_IOResultOk,
    C_IOResultEof,
    C_IOResultTimeout,
    C_IOResultCancelled,
    C_IOResultInvalid,
    C_IOResultError
} C_IOResultCode;

/**
 * @brief C coroutine result struct.
 * @details Resume paths do not pass business results as arguments. The resumed
 * coroutine reads result structs from its own request/task state.
 */
typedef struct C_IOResult {
    C_IOResultCode code;
    int sys_errno;
    size_t bytes;
    int64_t value;
    void* ptr;
} C_IOResult;

#ifdef __cplusplus
}
#endif

#endif
