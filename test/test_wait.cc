#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/async/AsyncFactory.h"
#include <iostream>

using namespace galay;

Coroutine<int> test1(CoSchedulerHandle handle)
{
    std::cout << "test1 start" << std::endl;
    AsyncFactory factory = handle.getAsyncFactory();
    co_await factory.getTimerGenerator().sleep(std::chrono::milliseconds(1000));
    std::cout << "test1 end" << std::endl;
    co_return 1;
}

WaitResult<int> wrapper(CoSchedulerHandle handle)
{
    return test1(handle).wait();
}

Coroutine<nil> test(CoSchedulerHandle handle)
{
    std::cout << "test start" << std::endl;
    auto res = co_await wrapper(handle);
    if(res.has_value()) {
        std::cout << "test success: " << res.value() << std::endl;
    } else {
        std::cout << "test failed" << std::endl;
    }
    co_return nil();
}

int main()
{
    RuntimeBuilder builder;
    Runtime runtime = builder.build();
    runtime.start();
    auto handle = runtime.getCoSchedulerHandle();
    handle.spawn(test(handle));
    getchar();
    runtime.stop();
    return 0;
}