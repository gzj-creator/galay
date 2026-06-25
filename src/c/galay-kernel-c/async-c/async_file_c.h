#ifndef GALAY_KERNEL_ASYNC_FILE_C_H
#define GALAY_KERNEL_ASYNC_FILE_C_H

#include "../core-c/runtime_c.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file async_file_c.h
 * @brief Galay kernel AsyncFile 的 C ABI 封装。
 *
 * @details 该头文件只暴露 C 可见的轻量句柄、状态码和回调结果。
 * 实际文件对象由实现文件中的 C++ galay::async::AsyncFile 承载。
 * AsyncFile 仅在 USE_KQUEUE 或 USE_IOURING 后端可用；其他后端会返回
 * C_AsyncFileOperationUnsupported。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AsyncFile C ABI 操作结果码。
 */
typedef enum C_AsyncFileResultCode {
    C_AsyncFileSuccess,                ///< 操作成功。
    C_AsyncFileParameterInvalid,       ///< 参数错误。
    C_AsyncFileMemoryAllocFailed,      ///< 内存或底层文件资源创建失败。
    C_AsyncFileIOFailed,               ///< 底层 IO 操作失败。
    C_AsyncFileOperationInvalid,       ///< 当前文件状态不允许执行该操作。
    C_AsyncFileOperationUnsupported,   ///< 当前 IO 后端不支持 AsyncFile。
    C_AsyncFileRuntimeNotRunning,      ///< runtime 未启动。
    C_AsyncFileRuntimeSpawnFailed,     ///< runtime 提交任务失败。
    C_AsyncFileTimeout,                ///< 操作超时。
} C_AsyncFileResultCode;

/**
 * @brief AsyncFile 文件打开模式。
 */
typedef enum C_AsyncFileOpenMode {
    C_AsyncFileOpenModeRead,       ///< 只读打开。
    C_AsyncFileOpenModeWrite,      ///< 只写打开，必要时创建文件。
    C_AsyncFileOpenModeReadWrite,  ///< 读写打开，必要时创建文件。
    C_AsyncFileOpenModeAppend,     ///< 追加写打开，必要时创建文件。
    C_AsyncFileOpenModeTruncate,   ///< 只写打开并截断文件，必要时创建文件。
} C_AsyncFileOpenMode;

/**
 * @brief AsyncFile C 句柄。
 *
 * @note file 指向内部 C++ AsyncFile 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_async_file {
    void* file;            ///< 内部 AsyncFile 对象指针。
} galay_kernel_async_file_t;

/**
 * @brief AsyncFile read 回调结果。
 */
typedef struct galay_kernel_async_file_read_result {
    C_AsyncFileResultCode code;     ///< read 结果码。
    char* buffer;                   ///< 调用 read 时传入的接收缓冲区。
    size_t length;                  ///< 调用 read 时请求的缓冲区长度。
    size_t offset;                  ///< 调用 read 时请求的文件偏移。
    size_t bytes;                   ///< 成功读取的字节数。
} galay_kernel_async_file_read_result_t;

/**
 * @brief AsyncFile write 回调结果。
 */
typedef struct galay_kernel_async_file_write_result {
    C_AsyncFileResultCode code;     ///< write 结果码。
    const char* buffer;             ///< 调用 write 时传入的发送缓冲区。
    size_t length;                  ///< 调用 write 时请求写入的字节数。
    size_t offset;                  ///< 调用 write 时请求的文件偏移。
    size_t bytes;                   ///< 成功写入的字节数。
} galay_kernel_async_file_write_result_t;

/**
 * @brief AsyncFile read 完成回调。
 *
 * @param result read 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_async_file_read 时传入的用户上下文。
 */
typedef void (*galay_kernel_async_file_read_callback_t)(
    galay_kernel_async_file_read_result_t* result,
    void* ctx);

/**
 * @brief AsyncFile write 完成回调。
 *
 * @param result write 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_async_file_write 时传入的用户上下文。
 */
typedef void (*galay_kernel_async_file_write_callback_t)(
    galay_kernel_async_file_write_result_t* result,
    void* ctx);

/**
 * @brief AsyncFile close 完成回调。
 *
 * @param code close 完成结果码；关闭成功为 C_AsyncFileSuccess，关闭失败为 C_AsyncFileIOFailed。
 * @param ctx 调用 galay_kernel_async_file_close 时传入的用户上下文。
 */
typedef void (*galay_kernel_async_file_close_callback_t)(
    C_AsyncFileResultCode code,
    void* ctx);

/**
 * @brief 将 AsyncFile 结果码转换为可读错误信息。
 *
 * @param code C_AsyncFileResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_async_file_get_error(C_AsyncFileResultCode code);

/**
 * @brief 创建 AsyncFile。
 *
 * @param c_file 输出文件句柄；成功时其 file 字段指向内部 AsyncFile。
 * @return 成功返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * 内存分配失败返回 C_AsyncFileMemoryAllocFailed；后端不支持返回 C_AsyncFileOperationUnsupported。
 *
 * @note 该函数只创建对象，不打开文件，也不会启动协程。
 */
C_AsyncFileResultCode galay_kernel_async_file_create(galay_kernel_async_file_t* c_file);

/**
 * @brief 销毁 AsyncFile 内部资源。
 *
 * @param c_file 由 galay_kernel_async_file_create 初始化的文件句柄。
 * @return 成功返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid。
 *
 * @note 该函数会释放 c_file->file 指向的内部 AsyncFile，并将其置空。非支持后端上
 * create 不会创建内部对象，destroy 对非空句柄执行置空 no-op 并返回 C_AsyncFileSuccess。
 */
C_AsyncFileResultCode galay_kernel_async_file_destroy(galay_kernel_async_file_t* c_file);

/**
 * @brief 以指定模式同步打开文件。
 *
 * @param c_file 由 galay_kernel_async_file_create 初始化的文件句柄。
 * @param path 文件系统路径，不能为空。
 * @param mode 文件打开模式。
 * @param permissions 创建文件时使用的权限，如 0644。
 * @return 成功返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * IO 失败返回 C_AsyncFileIOFailed；后端不支持返回 C_AsyncFileOperationUnsupported。
 *
 * @note 该函数只执行同步 open 设置，不启动协程。
 */
C_AsyncFileResultCode galay_kernel_async_file_open(
    galay_kernel_async_file_t* c_file,
    const char* path,
    C_AsyncFileOpenMode mode,
    int permissions);

/**
 * @brief 查询当前文件大小。
 *
 * @param c_file 已打开的 AsyncFile 句柄。
 * @param size 输出文件大小。
 * @return 成功返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * 当前状态不允许返回 C_AsyncFileOperationInvalid；IO 失败返回 C_AsyncFileIOFailed。
 */
C_AsyncFileResultCode galay_kernel_async_file_size(
    const galay_kernel_async_file_t* c_file,
    size_t* size);

/**
 * @brief 同步刷盘当前文件。
 *
 * @param c_file 已打开的 AsyncFile 句柄。
 * @return 成功返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * 当前状态不允许返回 C_AsyncFileOperationInvalid；IO 失败返回 C_AsyncFileIOFailed。
 */
C_AsyncFileResultCode galay_kernel_async_file_sync(galay_kernel_async_file_t* c_file);

/**
 * @brief 在 runtime 上异步读取文件。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file 已打开的 AsyncFile 句柄；必须存活到 callback 完成。
 * @param buffer 接收缓冲区；必须存活到 callback 完成。
 * @param length 接收缓冲区长度，必须大于 0。
 * @param offset 文件读取偏移。
 * @param callback read 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * runtime 未运行返回 C_AsyncFileRuntimeNotRunning；提交失败返回 C_AsyncFileRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待读取完成；最终读取字节数通过 callback 上报。
 */
C_AsyncFileResultCode galay_kernel_async_file_read(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    char* buffer,
    size_t length,
    size_t offset,
    galay_kernel_async_file_read_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步读取文件，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file 已打开的 AsyncFile 句柄；必须存活到 callback 完成。
 * @param buffer 接收缓冲区；必须存活到 callback 完成。
 * @param length 接收缓冲区长度，必须大于 0。
 * @param offset 文件读取偏移。
 * @param timeout_ms 超时时间，单位毫秒；0 表示立即超时检查。
 * @param callback read 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * runtime 未运行返回 C_AsyncFileRuntimeNotRunning；提交失败返回 C_AsyncFileRuntimeSpawnFailed；
 * 当前 IO 后端不支持返回 C_AsyncFileOperationUnsupported。
 *
 * @note 该函数不会阻塞等待读取完成；超时通过 callback 上报 C_AsyncFileTimeout，bytes 为 0。
 */
C_AsyncFileResultCode galay_kernel_async_file_read_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    char* buffer,
    size_t length,
    size_t offset,
    uint64_t timeout_ms,
    galay_kernel_async_file_read_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步写入文件。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file 已打开的 AsyncFile 句柄；必须存活到 callback 完成。
 * @param buffer 写入缓冲区；必须存活到 callback 完成。
 * @param length 写入字节数，必须大于 0。
 * @param offset 文件写入偏移。
 * @param callback write 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * runtime 未运行返回 C_AsyncFileRuntimeNotRunning；提交失败返回 C_AsyncFileRuntimeSpawnFailed。
 *
 * @note 该函数不会阻塞等待写入完成；最终写入字节数通过 callback 上报。
 */
C_AsyncFileResultCode galay_kernel_async_file_write(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    const char* buffer,
    size_t length,
    size_t offset,
    galay_kernel_async_file_write_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步写入文件，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file 已打开的 AsyncFile 句柄；必须存活到 callback 完成。
 * @param buffer 写入缓冲区；必须存活到 callback 完成。
 * @param length 写入字节数，必须大于 0。
 * @param offset 文件写入偏移。
 * @param timeout_ms 超时时间，单位毫秒；0 表示立即超时检查。
 * @param callback write 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * runtime 未运行返回 C_AsyncFileRuntimeNotRunning；提交失败返回 C_AsyncFileRuntimeSpawnFailed；
 * 当前 IO 后端不支持返回 C_AsyncFileOperationUnsupported。
 *
 * @note 该函数不会阻塞等待写入完成；超时通过 callback 上报 C_AsyncFileTimeout，bytes 为 0。
 */
C_AsyncFileResultCode galay_kernel_async_file_write_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    const char* buffer,
    size_t length,
    size_t offset,
    uint64_t timeout_ms,
    galay_kernel_async_file_write_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步关闭文件。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file 已打开的 AsyncFile 句柄；关闭后仍需调用 destroy 释放句柄对象。
 * @param callback close 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * runtime 未运行返回 C_AsyncFileRuntimeNotRunning；提交失败返回 C_AsyncFileRuntimeSpawnFailed。
 *
 * @note 该函数只关闭底层文件，不释放 c_file 句柄对象；最终关闭结果通过 callback 上报。
 */
C_AsyncFileResultCode galay_kernel_async_file_close(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    galay_kernel_async_file_close_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步关闭文件，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file 已打开的 AsyncFile 句柄；关闭后仍需调用 destroy 释放句柄对象。
 * @param timeout_ms 超时时间，单位毫秒；0 表示立即超时检查。
 * @param callback close 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AsyncFileSuccess；参数无效返回 C_AsyncFileParameterInvalid；
 * runtime 未运行返回 C_AsyncFileRuntimeNotRunning；提交失败返回 C_AsyncFileRuntimeSpawnFailed；
 * 当前 IO 后端不支持返回 C_AsyncFileOperationUnsupported。
 *
 * @note 该函数只关闭底层文件，不释放 c_file 句柄对象；超时通过 callback 上报 C_AsyncFileTimeout。
 */
C_AsyncFileResultCode galay_kernel_async_file_close_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    uint64_t timeout_ms,
    galay_kernel_async_file_close_callback_t callback,
    void* ctx);

#ifdef __cplusplus
}
#endif

#endif
