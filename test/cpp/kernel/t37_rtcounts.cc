/**
 * @file t37_rtcounts.cc
 * @brief 用途：验证 `RuntimeBuilder` 显式配置的调度器数量会被严格保留。
 * 关键覆盖点：IO 调度器数量、自动计算调度器哨兵值、运行时查询接口。
 * 通过条件：查询到的调度器数量与配置一致，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <cassert>
#include <iostream>

using namespace galay::kernel;

int main() {
    static_assert(GALAY_RUNTIME_SCHEDULER_COUNT_AUTO == static_cast<size_t>(-1),
                  "Auto scheduler count sentinel must map to size_t(-1)");

    {
        Runtime runtime = RuntimeBuilder()
            .io_scheduler_count(4)
            .parallel_scheduler_count(0)
            .build();

        runtime.start();
        assert(runtime.get_io_scheduler_count() == 4);
        assert(runtime.get_parallel_scheduler_count() == 0);
        runtime.stop();
    }

    {
        Runtime runtime = RuntimeBuilder()
            .io_scheduler_count(4)
            .parallel_scheduler_count(GALAY_RUNTIME_SCHEDULER_COUNT_AUTO)
            .build();

        runtime.start();
        assert(runtime.get_io_scheduler_count() == 4);
        assert(runtime.get_parallel_scheduler_count() >= 1);
        runtime.stop();
    }

    std::cout << "T37-RuntimeStrictSchedulerCounts PASS\n";
    return 0;
}
