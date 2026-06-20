/**
 * @file t46_blockres.cc
 * @brief 用途：验证 `Runtime::blockOn` 可以阻塞等待并返回 `Task` 结果。
 * 关键覆盖点：根任务执行、返回值传递、阻塞等待直到协程完成。
 * 通过条件：`blockOn` 返回值与预期一致且测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <cassert>
#include <iostream>

using namespace galay::kernel;

Task<int> answerTask()
{
    co_return 42;
}

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    auto value = runtime.blockOn(answerTask());
    assert(value.has_value());
    assert(*value == 42);

    std::cout << "T46-RuntimeBlockOnResult PASS\n";
    return 0;
}
