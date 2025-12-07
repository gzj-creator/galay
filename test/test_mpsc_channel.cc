#include "galay/kernel/concurrency/MpscChannel.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include <chrono>
#include <iostream>
#include <bitset>
using namespace galay;

std::bitset<100000000> bitset;
std::bitset<100000000> bitsetBatch;

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
        bitset[result.value()] = true;
        if(i == MAX_COUNT) {
            break;
        }
    }
    auto end_steady = std::chrono::steady_clock::now();
    auto duration_steady = end_steady - start_steady;
    
    // 直接获取纳秒
    auto ns_steady = std::chrono::duration_cast<std::chrono::nanoseconds>(duration_steady);
    std::cout << "steady_clock 耗时: " << ns_steady.count() << " ns" << std::endl;
    std::cout << "Consume finished, consumed: " << i << std::endl;
    
    // 验证所有数字都被接收
    bool all_received = true;
    for (int j = 0; j < MAX_COUNT; ++j) {
        if (!bitset[j]) {
            std::cout << "Missing number: " << j << std::endl;
            all_received = false;
            if (j > 10) {  // 只打印前几个缺失的数字
                break;
            }
        }
    }
    if (all_received) {
        std::cout << "All numbers (0 to " << (MAX_COUNT - 1) << ") received successfully!" << std::endl;
    }
    co_return nil();
}

Coroutine<nil> Produce(mpsc::AsyncChannel<int>& channel, int start, int end)
{
    for (int i = start; i < end; ++i) {
        int value = i;
        channel.send(std::move(value));
    }
    std::cout << "Produce finished: " << start << " to " << (end - 1) << std::endl;
    co_return nil();
}


Coroutine<nil> ConsumeBatch(mpsc::AsyncChannel<int>& channel)
{
    std::chrono::steady_clock::time_point start_steady;
    // 执行一些操作...
    int i = 0;
    while(true) {
        auto result = co_await channel.recvBatch();
        if(!result.has_value()) {
            std::cout << "no value" << std::endl;
            co_return nil();
        }
        for(auto& value : result.value()) {
            if(i == 0) start_steady = std::chrono::steady_clock::now();
            bitsetBatch[value] = true;
            ++i;
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
    std::cout << "ConsumeBatch finished, consumed: " << i << std::endl;
    
    // 验证所有数字都被接收
    bool all_received = true;
    for (int j = 0; j < MAX_COUNT; ++j) {
        if (!bitsetBatch[j]) {
            std::cout << "Missing number: " << j << std::endl;
            all_received = false;
            if (j > 10) {  // 只打印前几个缺失的数字
                break;
            }
        }
    }
    if (all_received) {
        std::cout << "All numbers (0 to " << (MAX_COUNT - 1) << ") received successfully!" << std::endl;
    }
    co_return nil();
}

Coroutine<nil> ProduceBatch(mpsc::AsyncChannel<int>& channel, int start, int end)
{
    std::vector<int> values(1000);
    // 计算需要发送的批次数量
    int total = end - start;
    int batch_count = total / 1000;
    int remainder = total % 1000;
    
    // 发送完整的批次
    for (int i = 0; i < batch_count; ++i) {
        for(int j = 0; j < 1000; ++j) {
            values[j] = start + i * 1000 + j;
        }
        channel.sendBatch(values);
    }
    
    // 发送剩余的元素
    if (remainder > 0) {
        values.resize(remainder);
        for(int j = 0; j < remainder; ++j) {
            values[j] = start + batch_count * 1000 + j;
        }
        channel.sendBatch(values);
    }
    
    std::cout << "ProduceBatch finished: " << start << " to " << (end - 1) << std::endl;
    co_return nil();
}

void test(CoSchedulerHandle handle, CoSchedulerHandle producer_handle1, CoSchedulerHandle producer_handle2)
{
    mpsc::AsyncChannel<int> channel;
    // 第一个生产者发送 0 到 MAX_COUNT/2 - 1
    // 第二个生产者发送 MAX_COUNT/2 到 MAX_COUNT - 1
    producer_handle1.spawn(Produce(channel, 0, MAX_COUNT / 2));
    producer_handle2.spawn(Produce(channel, MAX_COUNT / 2, MAX_COUNT));
    handle.spawn(Consume(channel));
    getchar();
}

void testBatch(CoSchedulerHandle handle, CoSchedulerHandle producer_handle1, CoSchedulerHandle producer_handle2)
{
    mpsc::AsyncChannel<int> channel;
    // 第一个生产者发送 0 到 MAX_COUNT/2 - 1
    // 第二个生产者发送 MAX_COUNT/2 到 MAX_COUNT - 1
    producer_handle1.spawn(ProduceBatch(channel, 0, MAX_COUNT / 2));
    producer_handle2.spawn(ProduceBatch(channel, MAX_COUNT / 2, MAX_COUNT));
    handle.spawn(ConsumeBatch(channel));
    getchar();
}

int main() {
    RuntimeBuilder builder;
    Runtime runtime = builder.build();
    runtime.start();

    // 重置 bitset
    bitset.reset();
    std::cout << "=== Testing single send/recv ===" << std::endl;
    test(runtime.getCoSchedulerHandle(0).value(), runtime.getCoSchedulerHandle(1).value(), runtime.getCoSchedulerHandle(2).value());

    // 重置 bitsetBatch
    bitsetBatch.reset();
    std::cout << "\n=== Testing batch send/recv ===" << std::endl;
    testBatch(runtime.getCoSchedulerHandle(0).value(), runtime.getCoSchedulerHandle(1).value(), runtime.getCoSchedulerHandle(2).value());
    runtime.stop();
    return 0;
}