#ifndef GALAY_KERNEL_CORE_C_CORO_AIO_FILE_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_AIO_FILE_BRIDGE_H

#include "c_coro_tcp_bridge.h"

#include <sys/types.h>

/**
 * @file c_coro_aio_file_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 AioFile commit adapter。
 *
 * @details 该内部 bridge 仅在 USE_EPOLL 后端可用；不支持时 direct API 返回
 * GalayCoreCoroIOResultError/sys_errno=ENOTSUP。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 提交 AioFile pending requests 并等待 commit 完成。
 *
 * @param file 内部 AioFile 指针，必须非 NULL 且处于有效状态。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param results 调用方提供的结果数组，容量至少为 result_capacity。
 * @param result_capacity results 可容纳的 ssize_t 条目数，必须大于 0。
 * @param out_count 输出实际写入 results 的条目数，必须非 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok，bytes/value 语义由实现填充，out_count 表示结果条目数；
 * 超时返回 Timeout；参数、scheduler、文件状态或 wait hook 无效返回 Invalid；
 * 底层错误返回 Error/sys_errno；不支持后端返回 Error/ENOTSUP。
 *
 * @note results、out_count 和 user_data 必须在函数返回或清理完成前保持有效。
 * 函数开始提交前会将 *out_count 置 0。
 */
GalayCoreCoroIOResult galay_core_coro_aio_file_commit(GalayCoreAioFile* file,
                                                      GalayCoreIOScheduler* scheduler,
                                                      ssize_t* results,
                                                      size_t result_capacity,
                                                      size_t* out_count,
                                                      int64_t timeout_ms,
                                                      void* user_data,
                                                      const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
