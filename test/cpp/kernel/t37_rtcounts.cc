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
            .ioSchedulerCount(4)
            .computeSchedulerCount(0)
            .build();

        runtime.start();
        assert(runtime.getIOSchedulerCount() == 4);
        assert(runtime.getComputeSchedulerCount() == 0);
        runtime.stop();
    }

    {
        Runtime runtime = RuntimeBuilder()
            .ioSchedulerCount(4)
            .computeSchedulerCount(GALAY_RUNTIME_SCHEDULER_COUNT_AUTO)
            .build();

        runtime.start();
        assert(runtime.getIOSchedulerCount() == 4);
        assert(runtime.getComputeSchedulerCount() >= 1);
        runtime.stop();
    }

    std::cout << "T37-RuntimeStrictSchedulerCounts PASS\n";
    return 0;
}
