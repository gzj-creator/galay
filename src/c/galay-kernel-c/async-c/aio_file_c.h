#ifndef GALAY_KERNEL_AIO_FILE_C_H
#define GALAY_KERNEL_AIO_FILE_C_H

#include "../core-c/runtime_c.h"
#include <stddef.h>
#include <sys/types.h>

/**
 * @file aio_file_c.h
 * @brief Galay kernel AioFile 的 C ABI 封装。
 *
 * @details 该头文件只暴露 C 可见的轻量句柄和结果码，实际文件对象生命周期
 * 由实现文件中的 C++ galay::async::AioFile 承载。AioFile 仅在 USE_EPOLL
 * 后端可用，其他后端的函数会返回 C_AioFileOperationUnsupported。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AioFile C ABI 操作结果码。
 */
typedef enum C_AioFileResultCode {
    C_AioFileSuccess,                 ///< 操作成功。
    C_AioFileParameterInvalid,        ///< 参数错误。
    C_AioFileMemoryAllocFailed,       ///< 内存分配失败。
    C_AioFileIOFailed,                ///< 底层 IO 操作失败。
    C_AioFileOperationInvalid,        ///< 当前文件状态不允许执行该操作。
    C_AioFileOperationUnsupported,    ///< 当前后端不支持 AioFile。
    C_AioFileRuntimeNotRunning,       ///< runtime 未启动。
    C_AioFileRuntimeSpawnFailed,      ///< runtime 提交任务失败。
} C_AioFileResultCode;

/**
 * @brief AioFile 打开模式。
 *
 * @note 所有模式在 USE_EPOLL 后端都会使用底层 AioFile 的 O_DIRECT 语义，
 * 调用方需要使用 galay_kernel_aio_file_alloc_aligned_buffer 分配对齐缓冲区。
 */
typedef enum C_AioFileOpenMode {
    C_AioFileOpenModeRead,        ///< 只读打开。
    C_AioFileOpenModeWrite,       ///< 只写打开，必要时创建文件。
    C_AioFileOpenModeReadWrite,   ///< 读写打开，必要时创建文件。
} C_AioFileOpenMode;

/**
 * @brief AioFile C 句柄。
 *
 * @note file 指向内部 C++ AioFile 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_aio_file {
    void* file;        ///< 内部 AioFile 对象指针。
} galay_kernel_aio_file_t;

/**
 * @brief AioFile commit 回调结果。
 *
 * @note results 指向的数组只在 callback 执行期间有效；需要保留时调用方必须复制。
 */
typedef struct galay_kernel_aio_file_commit_result {
    C_AioFileResultCode code;  ///< commit 结果码。
    size_t count;              ///< results 中返回值数量。
    ssize_t* results;          ///< 每个 AIO 操作的返回值；只在回调期间有效。
} galay_kernel_aio_file_commit_result_t;

/**
 * @brief AioFile commit 完成回调。
 *
 * @param result commit 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_aio_file_commit 时传入的用户上下文。
 */
typedef void (*galay_kernel_aio_file_commit_callback_t)(
    galay_kernel_aio_file_commit_result_t* result,
    void* ctx);

/**
 * @brief 将 AioFile 结果码转换为可读错误信息。
 *
 * @param code C_AioFileResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_aio_file_get_error(C_AioFileResultCode code);

/**
 * @brief 创建 AioFile。
 *
 * @param c_file 输出 AioFile 句柄；成功时其 file 字段指向内部 AioFile。
 * @param max_events AIO 队列深度，必须大于 0。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 内存分配失败返回 C_AioFileMemoryAllocFailed；非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 *
 * @note 底层 libaio/eventfd 初始化失败会在后续 open/commit 路径通过 IO 结果暴露；
 * 当前 C++ AioFile 构造函数本身不返回初始化错误。
 */
C_AioFileResultCode galay_kernel_aio_file_create(galay_kernel_aio_file_t* c_file, int max_events);

/**
 * @brief 销毁 AioFile 内部资源。
 *
 * @param c_file 由 galay_kernel_aio_file_create 初始化的 AioFile 句柄。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 *
 * @note 该函数会释放 c_file->file 指向的内部 AioFile，并将其置空。
 */
C_AioFileResultCode galay_kernel_aio_file_destroy(galay_kernel_aio_file_t* c_file);

/**
 * @brief 以指定模式打开文件。
 *
 * @param c_file AioFile 句柄。
 * @param path 文件路径，不能为空。
 * @param mode 文件打开模式。
 * @param permissions 创建文件时使用的权限。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * IO 失败返回 C_AioFileIOFailed；重复打开返回 C_AioFileOperationInvalid；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_open(
    galay_kernel_aio_file_t* c_file,
    const char* path,
    C_AioFileOpenMode mode,
    int permissions);

/**
 * @brief 预注册一个异步读操作，等待 commit 批量提交。
 *
 * @param c_file 已打开的 AioFile 句柄。
 * @param buffer 对齐的目标缓冲区；必须存活到 commit callback 完成。
 * @param length 读取字节数，必须大于 0。
 * @param offset 文件偏移量，不能为负数。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 文件未打开返回 C_AioFileOperationInvalid；非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_pre_read(
    galay_kernel_aio_file_t* c_file,
    char* buffer,
    size_t length,
    off_t offset);

/**
 * @brief 预注册一个异步写操作，等待 commit 批量提交。
 *
 * @param c_file 已打开的 AioFile 句柄。
 * @param buffer 对齐的源缓冲区；必须存活到 commit callback 完成。
 * @param length 写入字节数，必须大于 0。
 * @param offset 文件偏移量，不能为负数。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 文件未打开返回 C_AioFileOperationInvalid；非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_pre_write(
    galay_kernel_aio_file_t* c_file,
    const char* buffer,
    size_t length,
    off_t offset);

/**
 * @brief 清空已预注册但尚未提交的 AIO 操作。
 *
 * @param c_file AioFile 句柄。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_clear(galay_kernel_aio_file_t* c_file);

/**
 * @brief 关闭 AioFile 当前文件描述符。
 *
 * @param c_file AioFile 句柄；关闭后仍需调用 destroy 释放句柄对象。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_close(galay_kernel_aio_file_t* c_file);

/**
 * @brief 查询当前文件大小。
 *
 * @param c_file 已打开的 AioFile 句柄。
 * @param size 输出文件大小。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 文件未打开返回 C_AioFileOperationInvalid；IO 失败返回 C_AioFileIOFailed；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_size(
    galay_kernel_aio_file_t* c_file,
    size_t* size);

/**
 * @brief 将当前文件数据同步到稳定存储。
 *
 * @param c_file 已打开的 AioFile 句柄。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 文件未打开返回 C_AioFileOperationInvalid；IO 失败返回 C_AioFileIOFailed；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_sync(galay_kernel_aio_file_t* c_file);

/**
 * @brief 分配适用于 O_DIRECT AIO 操作的对齐缓冲区。
 *
 * @param size 缓冲区大小，必须大于 0。
 * @param alignment 对齐边界，必须是 2 的幂且不小于 sizeof(void*)。
 * @param buffer 输出缓冲区指针；成功后由 galay_kernel_aio_file_free_aligned_buffer 释放。
 * @return 成功返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * 分配失败返回 C_AioFileMemoryAllocFailed；非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_alloc_aligned_buffer(
    size_t size,
    size_t alignment,
    char** buffer);

/**
 * @brief 释放先前由 galay_kernel_aio_file_alloc_aligned_buffer 分配的缓冲区。
 *
 * @param buffer 待释放缓冲区；为空时为无操作。
 * @return 成功返回 C_AioFileSuccess；非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 */
C_AioFileResultCode galay_kernel_aio_file_free_aligned_buffer(char* buffer);

/**
 * @brief 在 runtime 上异步提交所有已预注册的 AIO 操作。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_file AioFile 句柄；必须存活到 callback 完成。
 * @param callback commit 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_AioFileSuccess；参数无效返回 C_AioFileParameterInvalid；
 * runtime 未运行返回 C_AioFileRuntimeNotRunning；提交失败返回 C_AioFileRuntimeSpawnFailed；
 * 非 USE_EPOLL 后端返回 C_AioFileOperationUnsupported。
 *
 * @note 该函数不会阻塞等待 AIO 完成；最终每个操作的 ssize_t 结果通过 callback 上报。
 */
C_AioFileResultCode galay_kernel_aio_file_commit(
    galay_kernel_runtime_t* runtime,
    galay_kernel_aio_file_t* c_file,
    galay_kernel_aio_file_commit_callback_t callback,
    void* ctx);

#ifdef __cplusplus
}
#endif

#endif
