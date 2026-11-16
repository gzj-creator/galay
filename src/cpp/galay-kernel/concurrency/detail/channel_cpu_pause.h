/**
 * @file channel_cpu_pause.h
 * @brief 异步通道共用的平台 CPU 自旋提示。
 */

#ifndef GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_CPU_PAUSE_H
#define GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_CPU_PAUSE_H

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

namespace galay::kernel::detail
{

/**
 * @brief 执行一次平台相关的短时 CPU 自旋提示。
 * @return 无返回值
 * @note 该函数不阻塞线程，也不主动让出操作系统时间片。Apple AArch64 使用
 *       ISB，避免高竞争场景下 YIELD 过度让步。
 */
inline void channel_cpu_pause() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__APPLE__) && defined(__aarch64__)
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    // 未提供平台 pause 指令时，退避窗口仍然只有数条指令。
#endif
}

/**
 * @brief 执行一次保留 Apple YIELD 语义的短时 CPU 自旋提示。
 * @return 无返回值
 * @note 两个无界通道历史上在 Apple AArch64 使用 YIELD；共享辅助实现时单独
 *       保留该平台行为，其他平台与 channel_cpu_pause() 相同。
 */
inline void channel_cpu_pause_yield_on_apple() noexcept
{
#if defined(__APPLE__) && defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    channel_cpu_pause();
#endif
}

} // namespace galay::kernel::detail

#endif // GALAY_KERNEL_CONCURRENCY_DETAIL_CHANNEL_CPU_PAUSE_H
