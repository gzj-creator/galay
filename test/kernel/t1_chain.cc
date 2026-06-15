/**
 * @file t1_chain.cc
 * @brief 用途：验证 `Task` 链式 `co_await` 调用在 `Runtime::blockOn` 根任务下的执行顺序。
 * 关键覆盖点：多层 `Task` 串联等待、嵌套恢复顺序、根任务阻塞等待到最终完成。
 * 通过条件：调用链按预期顺序执行，`blockOn` 正常返回且进程退出码为 0。
 */

#include "galay-kernel/core/runtime.h"
#include <cassert>
#include <iostream>

using namespace galay::kernel;


Task<void> test2()
{
    std::cout << "test2 wait" << std::endl;
    std::cout << "test2 end" <<std::endl;
    co_return;
}

Task<void> test1()
{
    std::cout << "test1 wait" << std::endl;
    auto result = co_await test2();
    assert(result.has_value());
    std::cout << "test1 end" <<std::endl;
    co_return;
}

Task<void> test()
{
    std::cout << "test wait" << std::endl;
    auto result = co_await test1();
    assert(result.has_value());
    std::cout << "test end" <<std::endl;
    co_return;
}

Task<void> rootTask()
{
    auto result = co_await test();
    assert(result.has_value());
}


int main()
{
    Runtime runtime;
    auto result = runtime.blockOn(rootTask());
    assert(result.has_value());
    return 0;
}
