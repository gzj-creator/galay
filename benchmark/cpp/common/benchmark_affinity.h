#ifndef GALAY_BENCHMARK_AFFINITY_H
#define GALAY_BENCHMARK_AFFINITY_H

#include "benchmark_environment.h"
#include <cstddef>

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
 * @param coreIndex 启动时选择的 CPU 集合中的索引；超出集合大小时取模。
 * @return 实际生效的放置方式，调用方必须据此标注结果可比性。
 *
 * @note 必须先在 main 调用 initializeBenchmarkEnvironment()。绑定失败立即
 *       终止压测，不输出伪装为已控制环境的样本；显式 none 返回 unsupported。
 */
inline ThreadPlacement pinCurrentThread(std::size_t coreIndex)
{
    if (!detail::environmentInitialized) {
        std::cerr << "GALAY_BENCH_ENV status=error operation=worker-before-initialization\n";
        std::exit(EXIT_FAILURE);
    }
    if (!detail::cpuBindingEnabled) {
        return ThreadPlacement::kUnsupported;
    }
    const unsigned target = detail::selectedCpus[coreIndex % detail::selectedCpus.size()];
    const auto actual = utils::CPU::bindCurrentThread(std::span(&target, 1));
    if (!actual || actual->size() != 1 || actual->front() != target) {
        std::cerr << "GALAY_BENCH_ENV status=error operation=worker-cpu-affinity cpu="
                  << target << " error=" << (actual ? 0 : actual.error().value()) << '\n';
        std::exit(EXIT_FAILURE);
    }
    return ThreadPlacement::kPinnedToCore;
}

}  // namespace galay::benchmark

#endif
