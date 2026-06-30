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

/**
 * @brief AsyncFile 生命周期操作结果码。
 *
 * @details create/open/size/sync/destroy 同步返回该枚举；read/write/close 返回
 * C_IOResult。未启用 USE_KQUEUE 或 USE_IOURING 时，同步生命周期函数返回
 * C_AsyncFileOperationUnsupported。
 */
typedef enum C_AsyncFileResultCode {
    C_AsyncFileSuccess,                ///< 操作成功。
    C_AsyncFileParameterInvalid,       ///< 参数错误或句柄未初始化。
    C_AsyncFileMemoryAllocFailed,      ///< 内存分配失败。
    C_AsyncFileIOFailed,               ///< 底层文件 I/O 失败。
    C_AsyncFileOperationInvalid,       ///< 当前文件状态不允许执行该操作。
    C_AsyncFileOperationUnsupported,   ///< 当前构建后端不支持 AsyncFile。
} C_AsyncFileResultCode;

/**
 * @brief AsyncFile 打开模式。
 *
 * @details mode 会转换为 C++ AsyncFile 的 OpenMode；permissions 由 open 调用透传给
 * 底层文件创建/打开逻辑。
 */
typedef enum C_AsyncFileOpenMode {
    C_AsyncFileOpenModeRead,       ///< 只读打开。
    C_AsyncFileOpenModeWrite,      ///< 只写打开。
    C_AsyncFileOpenModeReadWrite,  ///< 读写打开。
    C_AsyncFileOpenModeAppend,     ///< 追加写打开。
    C_AsyncFileOpenModeTruncate,   ///< 截断写打开。
} C_AsyncFileOpenMode;

/**
 * @brief AsyncFile C 句柄。
 *
 * @note file 指向内部 C++ AsyncFile 对象，调用方不能解引用或直接释放。生命周期
 * 由 create/destroy 管理。
 */
typedef struct galay_kernel_async_file {
    void* file;      ///< 内部 AsyncFile 对象指针。
} galay_kernel_async_file_t;

/**
 * @brief 将 AsyncFile 生命周期结果码转换为可读错误字符串。
 *
 * @param code AsyncFile 生命周期结果码。
 * @return 指向静态只读字符串的指针，调用方不得释放。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
const char* galay_kernel_async_file_get_error(C_AsyncFileResultCode code);

/**
 * @brief 创建 AsyncFile 句柄。
 *
 * @param c_file 输出 file 句柄；成功时写入内部 AsyncFile 指针。
 * @return 成功返回 C_AsyncFileSuccess；参数非法返回 C_AsyncFileParameterInvalid；
 * 分配失败返回 C_AsyncFileMemoryAllocFailed；当前后端不支持时返回
 * C_AsyncFileOperationUnsupported。
 *
 * @note 该函数同步执行。成功后调用方拥有 c_file->file，必须调用 destroy 释放。
 */
C_AsyncFileResultCode galay_kernel_async_file_create(galay_kernel_async_file_t* c_file);

/**
 * @brief 销毁 AsyncFile 句柄。
 *
 * @param c_file 由 create 初始化的 file 句柄。
 * @return 成功返回 C_AsyncFileSuccess；c_file 为 NULL 返回
 * C_AsyncFileParameterInvalid。
 *
 * @note 释放后会将 c_file->file 置为 NULL。调用方必须保证没有挂起的 read/write/close
 * 仍可能访问该文件对象。
 */
C_AsyncFileResultCode galay_kernel_async_file_destroy(galay_kernel_async_file_t* c_file);

/**
 * @brief 同步打开文件。
 *
 * @param c_file 已创建的 AsyncFile 句柄。
 * @param path 文件路径，必须是非空 C 字符串。
 * @param mode 打开模式，必须是 C_AsyncFileOpenMode 的有效值。
 * @param permissions 创建文件时使用的权限，必须非负。
 * @return 成功返回 C_AsyncFileSuccess；参数非法返回 C_AsyncFileParameterInvalid；
 * 底层打开失败返回 C_AsyncFileIOFailed/OperationInvalid；当前后端不支持时返回
 * C_AsyncFileOperationUnsupported。
 *
 * @note 该函数同步执行，可能触发文件系统操作并短暂阻塞。path 内容只在调用期间读取。
 */
C_AsyncFileResultCode galay_kernel_async_file_open(
    galay_kernel_async_file_t* c_file,
    const char* path,
    C_AsyncFileOpenMode mode,
    int permissions);

/**
 * @brief 同步查询文件大小。
 *
 * @param c_file 已打开的 AsyncFile 句柄。
 * @param size 输出文件大小，必须非 NULL。
 * @return 成功返回 C_AsyncFileSuccess；参数非法返回 C_AsyncFileParameterInvalid；
 * 文件未打开返回 C_AsyncFileOperationInvalid；底层查询失败返回 C_AsyncFileIOFailed；
 * 当前后端不支持时返回 C_AsyncFileOperationUnsupported。
 *
 * @note size 由调用方提供并拥有。该函数同步执行。
 */
C_AsyncFileResultCode galay_kernel_async_file_size(
    const galay_kernel_async_file_t* c_file,
    size_t* size);

/**
 * @brief 同步刷新文件内容到后端。
 *
 * @param c_file 已打开的 AsyncFile 句柄。
 * @return 成功返回 C_AsyncFileSuccess；参数非法返回 C_AsyncFileParameterInvalid；
 * 文件未打开返回 C_AsyncFileOperationInvalid；底层 sync 失败返回 C_AsyncFileIOFailed；
 * 当前后端不支持时返回 C_AsyncFileOperationUnsupported。
 *
 * @note 该函数可能执行阻塞式文件系统同步；不要在延迟敏感的 coroutine 路径中调用。
 */
C_AsyncFileResultCode galay_kernel_async_file_sync(galay_kernel_async_file_t* c_file);

/**
 * @brief 挂起当前 C coroutine 并异步读取文件。
 *
 * @param file 已打开的 AsyncFile 句柄。
 * @param buffer 调用方提供的输出缓冲区，必须非 NULL。
 * @param length 最多读取字节数，必须大于 0。
 * @param offset 文件偏移，必须能转换为平台 off_t 且非负。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为读取字节数；超时返回
 * C_IOResultTimeout；参数、未打开文件或 coroutine 上下文无效返回
 * C_IOResultInvalid；底层错误返回 C_IOResultError/sys_errno；当前后端不支持时返回
 * C_IOResultError 且 sys_errno 为 ENOTSUP。
 *
 * @note 必须在运行于 IO scheduler 的 C coroutine 内调用。buffer 必须在函数返回前
 * 保持有效。
 */
C_IOResult galay_kernel_async_file_read(
    galay_kernel_async_file_t* file,
    char* buffer,
    size_t length,
    int64_t offset,
    int64_t timeout_ms);

/**
 * @brief 挂起当前 C coroutine 并异步写入文件。
 *
 * @param file 已打开的 AsyncFile 句柄。
 * @param buffer 待写入数据，必须非 NULL。
 * @param length 待写入字节数，必须大于 0。
 * @param offset 文件偏移，必须能转换为平台 off_t 且非负。
 * @param timeout_ms 负数无限等待，0 立即返回 C_IOResultTimeout，正数为毫秒超时。
 * @return 成功返回 C_IOResultOk，result.bytes 为写入字节数；超时返回
 * C_IOResultTimeout；参数、未打开文件或 coroutine 上下文无效返回
 * C_IOResultInvalid；底层错误返回 C_IOResultError/sys_errno；当前后端不支持时返回
 * C_IOResultError 且 sys_errno 为 ENOTSUP。
 *
 * @note buffer 必须在函数返回前保持有效。一次调用不保证写满 length 字节，调用方
 * 需要根据 result.bytes 处理短写。
 */
C_IOResult galay_kernel_async_file_write(
    galay_kernel_async_file_t* file,
    const char* buffer,
    size_t length,
    int64_t offset,
    int64_t timeout_ms);

/**
 * @brief 在当前 IO scheduler 上关闭 AsyncFile。
 *
 * @param file 已创建的 AsyncFile 句柄。
 * @param timeout_ms 为 ABI 对称保留；当前实现只校验取值是否可转换为内部 timeout。
 * @return 成功返回 C_IOResultOk；参数或 coroutine 上下文无效返回 C_IOResultInvalid；
 * 底层 close 注册失败返回 C_IOResultError/sys_errno；当前后端不支持时返回
 * C_IOResultError 且 sys_errno 为 ENOTSUP。
 *
 * @note close 不销毁句柄；调用方仍需调用 destroy 释放内部对象。
 */
C_IOResult galay_kernel_async_file_close(
    galay_kernel_async_file_t* file,
    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
