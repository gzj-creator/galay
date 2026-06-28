#ifndef GALAY_KERNEL_CORO_RESULT_C_H
#define GALAY_KERNEL_CORO_RESULT_C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C 协程和 I/O 结果码。
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
 * @brief C 协程结果结构。
 * @details 恢复路径不会通过参数传递业务结果。被恢复的协程从自己的
 * request/task 状态中读取结果结构。
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
