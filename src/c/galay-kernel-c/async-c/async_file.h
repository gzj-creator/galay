#ifndef GALAY_C_KERNEL_ASYNC_ASYNC_FILE_H
#define GALAY_C_KERNEL_ASYNC_ASYNC_FILE_H

/**
 * @file async_file.h
 * @brief 原生 C 异步文件，直接使用 native scheduler，无 C++ bridge。
 *
 * @details 提供高性能的协程式文件 I/O：
 * - 直接 syscall + native scheduler
 * - 零 C++ 边界开销
 * - 完整的文件操作支持
 */

#include <galay/c/galay-kernel-c/core-c/io_controller.h>
#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 文件打开标志。
 */
typedef enum galay_c_file_flags {
    GALAY_C_FILE_READ = 0x01,      // 只读
    GALAY_C_FILE_WRITE = 0x02,     // 只写
    GALAY_C_FILE_RDWR = 0x03,      // 读写
    GALAY_C_FILE_CREATE = 0x04,    // 创建（配合 WRITE）
    GALAY_C_FILE_TRUNC = 0x08,     // 截断（配合 WRITE）
    GALAY_C_FILE_APPEND = 0x10,    // 追加（配合 WRITE）
} galay_c_file_flags_t;

/**
 * @brief 原生 C 异步文件。
 */
typedef struct galay_c_async_file_native {
    int fd;
    galay_c_io_controller_t controller;
    galay_c_io_scheduler_t* scheduler;
    int64_t position;              // 当前文件位置（用于 seek）
} galay_c_async_file_t;

/**
 * @brief 打开文件。
 *
 * @param out_file 输出文件结构。
 * @param path 文件路径。
 * @param flags 打开标志（GALAY_C_FILE_* 组合）。
 * @param mode 权限模式（如 0644），仅在创建文件时使用。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 */
C_IOResult galay_c_async_file_open(galay_c_async_file_t* out_file,
                                   const char* path,
                                   uint32_t flags,
                                   uint32_t mode);

/**
 * @brief 从文件读取数据（协程挂起直到读取完成或超时）。
 *
 * @param file 异步文件。
 * @param buffer 接收缓冲区。
 * @param length 最多读取字节数。
 * @param timeout_ms 超时毫秒数；负数表示无限等待。
 * @return 成功返回 C_IOResultOk 且 bytes 字段为读取字节数；
 * EOF 返回 C_IOResultEof；超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 */
C_IOResult galay_c_async_file_read(galay_c_async_file_t* file,
                                          char* buffer,
                                          size_t length,
                                          int64_t timeout_ms);

/**
 * @brief 向文件写入数据（协程挂起直到写入完成或超时）。
 *
 * @param file 异步文件。
 * @param buffer 待写入数据。
 * @param length 待写入字节数。
 * @param timeout_ms 超时毫秒数；负数表示无限等待。
 * @return 成功返回 C_IOResultOk 且 bytes 字段为写入字节数；
 * 超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 */
C_IOResult galay_c_async_file_write(galay_c_async_file_t* file,
                                           const char* buffer,
                                           size_t length,
                                           int64_t timeout_ms);

/**
 * @brief 设置文件读写位置。
 *
 * @param file 异步文件。
 * @param offset 偏移量。
 * @param whence 起始位置：0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END。
 * @return 成功返回 C_IOResultOk 且 value 字段为新位置；失败返回对应错误码。
 */
C_IOResult galay_c_async_file_seek(galay_c_async_file_t* file,
                                          int64_t offset,
                                          int whence);

/**
 * @brief 获取当前文件位置。
 *
 * @param file 异步文件。
 * @return 成功返回 C_IOResultOk 且 value 字段为当前位置；失败返回对应错误码。
 */
C_IOResult galay_c_async_file_tell(galay_c_async_file_t* file);

/**
 * @brief 关闭文件。
 *
 * @param file 异步文件。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 *
 * @note 会取消挂起在该文件上的所有协程。
 */
C_IOResult galay_c_async_file_close(galay_c_async_file_t* file);

#ifdef __cplusplus
}
#endif

#endif
