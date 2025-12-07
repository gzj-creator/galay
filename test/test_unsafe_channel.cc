#include "galay/kernel/concurrency/UnsafeChannel.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include <iostream>

using namespace galay;

#define MAX_COUNT 100000000

Coroutine<nil> testSend(unsafe::AsyncChannel<int>& channel)
{
    for(int i = 0; i < MAX_COUNT; i++) {
        channel.send(i);
    }
    co_return nil();
}

Coroutine<nil> testRecv(unsafe::AsyncChannel<int>& channel)
{
    std::chrono::steady_clock::time_point start_steady;
    int i = 0;
    while (true) {
        auto result = co_await channel.recv();
        if(result) {
            if(i == 0) start_steady = std::chrono::steady_clock::now();
            ++i;
            if(i == MAX_COUNT) {
                break;
            }
        } else {
            std::cout << "recv failed" << std::endl;
        }
    }
    auto end_steady = std::chrono::steady_clock::now();
    auto duration_steady = end_steady - start_steady;
    auto ns_steady = std::chrono::duration_cast<std::chrono::nanoseconds>(duration_steady);
    std::cout << "steady_clock 耗时: " << ns_steady.count() << " ns" << std::endl;
    std::cout << "recv finished, received: " << i << std::endl;
    co_return nil();
}

void test(CoSchedulerHandle handle)
{
    unsafe::AsyncChannel<int> channel;
    handle.spawn(testRecv(channel));
    handle.spawn(testSend(channel));
    getchar();
    return;
}

int main()
{
    RuntimeBuilder builder;
    Runtime runtime = builder.build();
    runtime.start();
    CoSchedulerHandle handle = runtime.getCoSchedulerHandle();
    test(handle);
    runtime.stop();
    return 0;
}