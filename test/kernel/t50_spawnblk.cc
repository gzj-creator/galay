/**
 * @file t50_spawnblk.cc
 * @brief 用途：验证 `Runtime::spawnBlocking` 可在线程池中执行阻塞任务。
 * 关键覆盖点：阻塞 callable 提交、异步等待结果、与运行时线程分离执行。
 * 通过条件：阻塞任务结果正确返回，测试返回 0。
 */

#include "galay-kernel/core/runtime.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    auto start = std::chrono::steady_clock::now();

    auto first = runtime.spawnBlocking([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 11;
    });
    auto second = runtime.spawnBlocking([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 31;
    });
    assert(first.has_value());
    assert(second.has_value());

    const auto first_result = first->join();
    const auto second_result = second->join();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    assert(first_result.has_value());
    assert(second_result.has_value());
    assert(*first_result == 11);
    assert(*second_result == 31);
    assert(elapsed_ms < 190 && "spawnBlocking tasks should execute concurrently");

    runtime.stop();

    std::cout << "T50-RuntimeSpawnBlocking PASS\n";
    return 0;
}
