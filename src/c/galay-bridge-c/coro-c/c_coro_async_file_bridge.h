#ifndef GALAY_KERNEL_CORE_C_CORO_ASYNC_FILE_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_ASYNC_FILE_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_async_file_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AsyncFile adapter。
 *
 * @details 该内部 bridge 仅在 USE_KQUEUE 或 USE_IOURING 后端可用；不支持时 direct
 * API 返回 GalayCoreCoroIOResultError/sys_errno=ENOTSUP。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 提交 AsyncFile read awaitable 并等待完成。
 *
 * @param file 内部 AsyncFile 指针，必须非 NULL 且已打开。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param buffer 输出缓冲区，必须非 NULL。
 * @param length 最多读取字节数，必须大于 0。
 * @param offset 文件偏移，必须非负。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为读取字节数；超时返回 Timeout；参数、scheduler
 * 或 wait hook 无效返回 Invalid；底层错误返回 Error/sys_errno；不支持后端返回
 * Error/ENOTSUP。
 *
 * @note buffer/user_data 必须在函数返回或清理完成前保持有效。
 */
GalayCoreCoroIOResult galay_core_coro_async_file_read(GalayCoreAsyncFile* file,
                                                      GalayCoreIOScheduler* scheduler,
                                                      char* buffer,
                                                      size_t length,
                                                      int64_t offset,
                                                      int64_t timeout_ms,
                                                      void* user_data,
                                                      const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 AsyncFile write awaitable 并等待完成。
 *
 * @param file 内部 AsyncFile 指针，必须非 NULL 且已打开。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param buffer 待写入数据，必须非 NULL。
 * @param length 待写入字节数，必须大于 0。
 * @param offset 文件偏移，必须非负。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为写入字节数；超时返回 Timeout；参数、scheduler
 * 或 wait hook 无效返回 Invalid；底层错误返回 Error/sys_errno；不支持后端返回
 * Error/ENOTSUP。
 *
 * @note buffer/user_data 必须在函数返回前保持有效。调用方需要处理短写。
 */
GalayCoreCoroIOResult galay_core_coro_async_file_write(GalayCoreAsyncFile* file,
                                                       GalayCoreIOScheduler* scheduler,
                                                       const char* buffer,
                                                       size_t length,
                                                       int64_t offset,
                                                       int64_t timeout_ms,
                                                       void* user_data,
                                                       const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 在 IOScheduler 上关闭 AsyncFile。
 *
 * @param file 内部 AsyncFile 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须与 file controller 所属 scheduler 匹配。
 * @param timeout_ms 为 ABI 对称保留；当前实现不等待 timeout，只执行 close 注册。
 * @return 成功返回 Ok；参数或 scheduler 无效返回 Invalid；close 注册失败返回
 * Error/sys_errno；不支持后端返回 Error/ENOTSUP。
 *
 * @note close 不释放 AsyncFile 对象，调用方仍负责销毁外层句柄。
 */
GalayCoreCoroIOResult galay_core_coro_async_file_close(GalayCoreAsyncFile* file,
                                                       GalayCoreIOScheduler* scheduler,
                                                       int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
