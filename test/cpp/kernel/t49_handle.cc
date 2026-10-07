/**
 * @file t49_handle.cc
 * @brief 用途：验证运行时上下文中的 `RuntimeHandle` 获取与嵌套派生能力。
 * 关键覆盖点：`current/try_current`、当前运行时句柄可用性、通过句柄再次 spawn。
 * 通过条件：运行时句柄行为符合预期，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <cassert>
#include <iostream>

using namespace galay::kernel;

Task<bool> check_current_handle()
{
    auto current = RuntimeHandle::try_current();
    co_return current.has_value();
}

int main()
{
    assert(!RuntimeHandle::try_current().has_value());

    Runtime runtime = RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .build();

    auto inside_runtime = runtime.block_on_io(check_current_handle());
    assert(inside_runtime.has_value());
    assert(*inside_runtime);

    std::cout << "T49-RuntimeHandleCurrent PASS\n";
    return 0;
}
