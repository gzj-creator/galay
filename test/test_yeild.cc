#include "galay/kernel/coroutine/Waker.h"
#include "galay/kernel/runtime/Runtime.h"
#include <cstdio>

using namespace galay;

Coroutine<int> printA()
{
    for (int i = 0; i < 10; ++i) {
        std::cout << "A" << std::endl;
        co_yield 1;
    }
    co_return 0;
}

Coroutine<int> printB()
{
    for (int i = 0; i < 10; ++i) {
        std::cout << "B" << std::endl;
        co_yield 2;
    }
    co_return 0;
}

Coroutine<int> printC()
{
    for (int i = 0; i < 10; ++i) {
        std::cout << "C" << std::endl;
        co_yield 3;
    }
    co_return 0;
}

Coroutine<nil> sync(Runtime& runtime)
{
    runtime.schedule(printA(), 0);
    runtime.schedule(printB(), 0);
    runtime.schedule(printC(), 0);
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