#ifndef GALAY_C_KERNEL_ASYNC_AIO_FILE_H
#define GALAY_C_KERNEL_ASYNC_AIO_FILE_H

/**
 * @file aio_file.h
 * @brief 原生 C AIO 文件，使用 io_uring (Linux 5.1+) 或回退到 aio。
 *
 * @details 提供真正的异步文件 I/O（不阻塞线程）：
 * - io_uring 优先（高性能）
 * - aio 回退（兼容性）
 * - 与 native scheduler 集成
 */

#include <galay/c/galay-kernel-c/coro-c/coro_result.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief AIO 后端类型。
 */
typedef enum galay_c_aio_backend {
    GALAY_C_AIO_BACKEND_IOURING = 1,   // io_uring (Linux 5.1+)
    GALAY_C_AIO_BACKEND_AIO = 2,       // POSIX aio
    GALAY_C_AIO_BACKEND_FALLBACK = 3,  // 线程池模拟（最后的回退）
} galay_c_aio_backend_t;

/**
 * @brief AIO 文件打开标志。
 */
typedef enum galay_c_aio_flags {
    GALAY_C_AIO_READ = 0x01,
    GALAY_C_AIO_WRITE = 0x02,
    GALAY_C_AIO_RDWR = 0x03,
    GALAY_C_AIO_CREATE = 0x04,
    GALAY_C_AIO_TRUNC = 0x08,
    GALAY_C_AIO_DIRECT = 0x10,         // O_DIRECT（绕过页缓存）
} galay_c_aio_flags_t;

/**
 * @brief 原生 C AIO 文件。
 */
typedef struct galay_c_aio_file_native {
    int fd;
    galay_c_aio_backend_t backend;
    void* backend_context;             // 后端特定上下文
    int64_t position;
} galay_c_aio_file_t;

/**
 * @brief 检查 AIO 支持情况。
 *
 * @param out_backend 输出检测到的最佳后端类型。
 * @return 成功返回 C_IOResultOk；系统不支持 AIO 返回 C_IOResultError。
 *
 * @note 按优先级检测：io_uring > aio > 线程池回退。
 */
C_IOResult galay_c_aio_check_support(galay_c_aio_backend_t* out_backend);

/**
 * @brief 打开 AIO 文件。
 *
 * @param out_file 输出文件结构。
 * @param path 文件路径。
 * @param flags 打开标志（GALAY_C_AIO_* 组合）。
 * @param mode 权限模式（如 0644），仅在创建文件时使用。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 */
C_IOResult galay_c_aio_file_open(galay_c_aio_file_t* out_file,
                                        const char* path,
                                        uint32_t flags,
                                        uint32_t mode);

/**
 * @brief 异步读取数据（真正的异步，不阻塞线程）。
 *
 * @param file AIO 文件。
 * @param buffer 接收缓冲区（必须对齐，如使用 O_DIRECT）。
 * @param length 读取字节数。
 * @param offset 文件偏移量（-1 表示当前位置）。
 * @param timeout_ms 超时毫秒数；负数表示无限等待。
 * @return 成功返回 C_IOResultOk 且 bytes 字段为读取字节数；
 * EOF 返回 C_IOResultEof；超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 *
 * @note 该函数挂起协程但不阻塞线程。I/O 由内核或线程池异步完成。
 */
C_IOResult galay_c_aio_file_read(galay_c_aio_file_t* file,
                                        char* buffer,
                                        size_t length,
                                        int64_t offset,
                                        int64_t timeout_ms);

/**
 * @brief 异步写入数据（真正的异步，不阻塞线程）。
 *
 * @param file AIO 文件。
 * @param buffer 待写入数据（必须对齐，如使用 O_DIRECT）。
 * @param length 写入字节数。
 * @param offset 文件偏移量（-1 表示当前位置）。
 * @param timeout_ms 超时毫秒数；负数表示无限等待。
 * @return 成功返回 C_IOResultOk 且 bytes 字段为写入字节数；
 * 超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 */
C_IOResult galay_c_aio_file_write(galay_c_aio_file_t* file,
                                         const char* buffer,
                                         size_t length,
                                         int64_t offset,
                                         int64_t timeout_ms);

/**
 * @brief 异步 fsync（确保数据写入磁盘）。
 *
 * @param file AIO 文件。
 * @param timeout_ms 超时毫秒数；负数表示无限等待。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 */
C_IOResult galay_c_aio_file_fsync(galay_c_aio_file_t* file,
                                         int64_t timeout_ms);

/**
 * @brief 设置文件读写位置。
 *
 * @param file AIO 文件。
 * @param offset 偏移量。
 * @param whence 起始位置：0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END。
 * @return 成功返回 C_IOResultOk 且 value 字段为新位置；失败返回对应错误码。
 */
C_IOResult galay_c_aio_file_seek(galay_c_aio_file_t* file,
                                        int64_t offset,
                                        int whence);

/**
 * @brief 关闭 AIO 文件。
 *
 * @param file AIO 文件。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 *
 * @note 会等待所有挂起的 I/O 操作完成。
 */
C_IOResult galay_c_aio_file_close(galay_c_aio_file_t* file);

#ifdef __cplusplus
}
#endif

#endif
