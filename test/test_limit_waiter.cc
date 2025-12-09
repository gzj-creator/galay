#include "galay/common/Error.h"
#include "galay/kernel/coroutine/LimitWaiter.hpp"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include <iostream>
#include <memory>

using namespace galay;

Coroutine<nil> test1(CoSchedulerHandle handle, std::shared_ptr<LimitWaiter<nil, Infallible>> waiter)
{
    std::cout << "test1" << std::endl;
    auto factory = handle.getAsyncFactory();
    co_await factory.getTimerGenerator().sleep(std::chrono::milliseconds(1000));
    if(waiter->notify(nil())) {
        std::cout << "test1 notify success" << std::endl;
    } else {
        std::cout << "test1 notify failed" << std::endl;
    }
    co_return nil();
}

Coroutine<nil> test2(CoSchedulerHandle handle, std::shared_ptr<LimitWaiter<nil, Infallible>> waiter)
{
    std::cout << "test2" << std::endl;
    auto factory = handle.getAsyncFactory();
    co_await factory.getTimerGenerator().sleep(std::chrono::milliseconds(10000));
    if(waiter->notify(nil())) {
        std::cout << "test2 notify success" << std::endl;
    } else {
        std::cout << "test2 notify failed" << std::endl;
    }
    co_return nil();
}

Coroutine<nil> test(CoSchedulerHandle handle)
{
    std::shared_ptr<LimitWaiter<nil, Infallible>> waiter = std::make_shared<LimitWaiter<nil, Infallible>>();
    auto co1 = test1(handle, waiter);
    auto co2 = test2(handle, waiter);
    co2.then(co1.origin());
    waiter->appendTask(std::move(co2));
    auto result = co_await waiter->wait();
    if(result) {
        std::cout << "test success" << std::endl;
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
    runtime.schedule(test(runtime.getCoSchedulerHandle(0).value()));
    getchar();
    runtime.stop();
    return 0;
}