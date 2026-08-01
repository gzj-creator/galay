#ifndef GALAY_BENCHMARK_AFFINITY_H
#define GALAY_BENCHMARK_AFFINITY_H

#include <cstddef>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace galay::benchmark {

/**
 * @brief 线程放置的实际生效方式。
 * @details 用于在输出中标注本轮数据的可比性：只有 kPinnedToCore 才是真正绑核，
 *          kPerformanceClassOnly 仅保证留在高性能核簇上，核间迁移依然可能发生。
 */
enum class ThreadPlacement {
    kPinnedToCore,            ///< 已绑定到指定逻辑核（Linux）
    kPerformanceClassOnly,    ///< 仅限定在高性能核簇（Apple silicon 无法绑核）
    kAffinityHintOnly,        ///< 仅设置了调度亲和标签，不代表绑定到指定核心
    kUnsupported              ///< 平台不提供任何放置控制
};

/**
 * @brief 返回放置方式的可读名称。
 * @param placement 放置方式。
 * @return 静态字符串，覆盖所有枚举值。
 */
inline const char* threadPlacementName(ThreadPlacement placement) noexcept
{
    switch (placement) {
    case ThreadPlacement::kPinnedToCore:
        return "pinned";
    case ThreadPlacement::kPerformanceClassOnly:
        return "perf-class-only";
    case ThreadPlacement::kAffinityHintOnly:
        return "affinity-hint-only";
    case ThreadPlacement::kUnsupported:
        return "unsupported";
    }
    return "unknown";
}

/**
 * @brief 把当前线程固定到基准测试用的执行资源上。
 * @param coreIndex 期望的逻辑核序号；会按在线核数取模。
 * @return 实际生效的放置方式，调用方必须据此标注结果可比性。
 *
 * @note Linux 通过 pthread_setaffinity_np 真正绑核。
 * @note Darwin 的 THREAD_AFFINITY_POLICY 只是调度亲和标签，不是 CPU 绑定；
 *       QoS 成功时仅报告 kPerformanceClassOnly，不会把标签误报为 pinned。
 */
inline ThreadPlacement pinCurrentThread(std::size_t coreIndex) noexcept
{
    const unsigned online = std::thread::hardware_concurrency();
    const std::size_t target = online == 0 ? 0 : coreIndex % online;

#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(target), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        return ThreadPlacement::kPinnedToCore;
    }
    return ThreadPlacement::kUnsupported;
#elif defined(__APPLE__)
    // Darwin affinity tag 只提示调度器把相关线程放在不同资源组，不绑定逻辑核。
    thread_affinity_policy_data_t policy = {static_cast<integer_t>(target + 1)};
    const kern_return_t affinityHint = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_AFFINITY_POLICY,
        reinterpret_cast<thread_policy_t>(&policy),
        THREAD_AFFINITY_POLICY_COUNT);
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0) {
        return ThreadPlacement::kPerformanceClassOnly;
    }
    return affinityHint == KERN_SUCCESS
        ? ThreadPlacement::kAffinityHintOnly
        : ThreadPlacement::kUnsupported;
#else
    (void)target;
    return ThreadPlacement::kUnsupported;
#endif
}

}  // namespace galay::benchmark

#endif
