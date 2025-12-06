#include "galay/kernel/concurrency/MpscChannel.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include <chrono>
#include <iostream>

using namespace galay;

#define MAX_COUNT 100000000

Coroutine<nil> Consume(mpsc::AsyncChannel<int>& channel)
{
    std::chrono::steady_clock::time_point start_steady;
    // 执行一些操作...
    int i = 0;
    while(true) {
        auto result = co_await channel.recv();
        if(i == 0) start_steady = std::chrono::steady_clock::now();
        ++i;
        if(!result.has_value()) {
            std::cout << "no value" << std::endl;
            co_return nil();
        }
        if(i == MAX_COUNT) {
            break;
        }
    }
    auto end_steady = std::chrono::steady_clock::now();
    auto duration_steady = end_steady - start_steady;
    
    // 直接获取纳秒
    auto ns_steady = std::chrono::duration_cast<std::chrono::nanoseconds>(duration_steady);
    std::cout << "steady_clock 耗时: " << ns_steady.count() << " ns" << std::endl;
    std::cout << "Consume finished" << std::endl;
    co_return nil();
}

Coroutine<nil> Produce(mpsc::AsyncChannel<int>& channel)
{
    int i = 0;
    for (int i = 0; i <= MAX_COUNT / 2; ++i) {
        channel.send(i);
    }
    std::cout << "Produce finished" << std::endl;
    co_return nil();
}

void test(CoSchedulerHandle handle, CoSchedulerHandle producer_handle1, CoSchedulerHandle producer_handle2)
{
    mpsc::AsyncChannel<int> channel;
    producer_handle1.spawn(Produce(channel));
    producer_handle2.spawn(Produce(channel));
    handle.spawn(Consume(channel));
    getchar();
}

int main() {
    RuntimeBuilder builder;
    Runtime runtime = builder.build();
    runtime.start();

    test(runtime.getCoSchedulerHandle(0).value(), runtime.getCoSchedulerHandle(1).value(), runtime.getCoSchedulerHandle(2).value());

    
    runtime.stop();
    return 0;
}