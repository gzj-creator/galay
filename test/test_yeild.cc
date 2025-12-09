#include "galay/kernel/runtime/Runtime.h"
#include <cstdio>
#include <iostream>

using namespace galay;

Coroutine<int> printA()
{
    for (int i = 0; i < 10; ++i) {
        std::cout << "A" << std::endl;
        co_yield {true};
    }
    co_return 0;
}

Coroutine<int> printB()
{
    for (int i = 0; i < 10; ++i) {
        std::cout << "B" << std::endl;
        co_yield {true};
    }
    co_return 0;
}

Coroutine<int> printC()
{
    for (int i = 0; i < 10; ++i) {
        std::cout << "C" << std::endl;
        co_yield {true};
    }
    co_return 0;
}


Coroutine<nil> sync(Runtime& runtime)
{
    CoSchedulerHandle handle = runtime.getCoSchedulerHandle(0).value();
    handle.spawn(printA());
    handle.spawn(printB());
    handle.spawn(printC());
    co_return nil();
}

int main() {
    RuntimeBuilder builder;
    Runtime runtime = builder.build();
    runtime.start();
    runtime.schedule(sync(runtime));
    getchar();
    runtime.stop();
    return 0;
}