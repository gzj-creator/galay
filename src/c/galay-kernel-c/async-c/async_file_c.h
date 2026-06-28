#ifndef GALAY_KERNEL_ASYNC_FILE_C_H
#define GALAY_KERNEL_ASYNC_FILE_C_H

#include "../coro-c/coro_result_c.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file async_file_c.h
 * @brief Galay kernel AsyncFile 的 direct C coroutine ABI。
 *
 * @details AsyncFile 仅在 USE_KQUEUE 或 USE_IOURING 后端可用；不支持的后端
 * 通过生命周期函数返回 C_AsyncFileOperationUnsupported，异步 direct API 返回
 * C_IOResultError/sys_errno=ENOTSUP。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum C_AsyncFileResultCode {
    C_AsyncFileSuccess,
    C_AsyncFileParameterInvalid,
    C_AsyncFileMemoryAllocFailed,
    C_AsyncFileIOFailed,
    C_AsyncFileOperationInvalid,
    C_AsyncFileOperationUnsupported,
} C_AsyncFileResultCode;

typedef enum C_AsyncFileOpenMode {
    C_AsyncFileOpenModeRead,
    C_AsyncFileOpenModeWrite,
    C_AsyncFileOpenModeReadWrite,
    C_AsyncFileOpenModeAppend,
    C_AsyncFileOpenModeTruncate,
} C_AsyncFileOpenMode;

typedef struct galay_kernel_async_file {
    void* file;
} galay_kernel_async_file_t;

const char* galay_kernel_async_file_get_error(C_AsyncFileResultCode code);

C_AsyncFileResultCode galay_kernel_async_file_create(galay_kernel_async_file_t* c_file);

C_AsyncFileResultCode galay_kernel_async_file_destroy(galay_kernel_async_file_t* c_file);

C_AsyncFileResultCode galay_kernel_async_file_open(
    galay_kernel_async_file_t* c_file,
    const char* path,
    C_AsyncFileOpenMode mode,
    int permissions);

C_AsyncFileResultCode galay_kernel_async_file_size(
    const galay_kernel_async_file_t* c_file,
    size_t* size);

C_AsyncFileResultCode galay_kernel_async_file_sync(galay_kernel_async_file_t* c_file);

C_IOResult galay_kernel_async_file_read(
    galay_kernel_async_file_t* file,
    char* buffer,
    size_t length,
    int64_t offset,
    int64_t timeout_ms);

C_IOResult galay_kernel_async_file_write(
    galay_kernel_async_file_t* file,
    const char* buffer,
    size_t length,
    int64_t offset,
    int64_t timeout_ms);

C_IOResult galay_kernel_async_file_close(
    galay_kernel_async_file_t* file,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
