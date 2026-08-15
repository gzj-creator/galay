#ifndef GALAY_C_KERNEL_CORO_SLEEP_H
#define GALAY_C_KERNEL_CORO_SLEEP_H

/**
 * @file coro_sleep.h
 * @brief 原生 C 协程 sleep 实现。
 *
 * @details 提供协程友好的定时器功能：
 * - 不阻塞线程的 sleep
 * - 集成 native scheduler
 */

#include <galay/c/galay-kernel-c/coro-c/coro_result.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂起当前 C 协程指定时间。
 *
 * @param timeout_ms 休眠毫秒数；0 表示立即返回，负数无效。
 * @return 成功休眠返回 C_IOResultOk；不在协程内或参数无效返回 C_IOResultInvalid；
 * 定时器错误返回 C_IOResultError。
 *
 * @note 该函数挂起协程，不阻塞 scheduler 线程。
 *
 * @example
 * void my_coro(void* arg) {
 *     printf("Starting...\n");
 *     (void)galay_c_coro_sleep(1000);
 *     printf("After 1 second\n");
 * }
 */
C_IOResult galay_c_coro_sleep(int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
