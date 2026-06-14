/**
 * @file t47_blockex.cc
 * @brief 用途：验证 `Runtime::blockOn` 会把根任务异常转换为 RuntimeError。
 * 关键覆盖点：根任务异常抛出、expected 错误分支、错误码保持。
 * 通过条件：预期 RuntimeError 被成功返回，测试返回 0。
 */

#include "kernel/kernel/runtime.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace galay::kernel;

Task<int> explodeTask()
{
    throw std::runtime_error("boom");
    co_return 0;
}

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    auto result = runtime.blockOn(explodeTask());
    assert(!result.has_value());
    assert(result.error().code() == RuntimeErrorCode::kTaskException);

    std::cout << "T47-RuntimeBlockOnExpectedError PASS\n";
    return 0;
}
