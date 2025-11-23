#include "galay/kernel/coroutine/AsyncQueue.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/common/Error.h"
#include <iostream>
#include <chrono>

using namespace galay;
using namespace std::chrono_literals;

// 生产者协程
Coroutine<nil> producer(CoSchedulerHandle handle, std::shared_ptr<AsyncQueue<int, CommonError>> queue, int producerId, int itemCount) {
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    std::cout << "[Producer " << producerId << "] Starting production of " << itemCount << " items" << std::endl;

    for (int i = 0; i < itemCount; ++i) {
        // 模拟生产过程
        co_await generator.sleep(300ms);
        int value = producerId * 100 + i;

        std::cout << "[Producer " << producerId << "] Producing item " << i << ": " << value
                  << ", queue size: " << queue->size() << std::endl;
        queue->push(value);
    }

    std::cout << "[Producer " << producerId << "] Finished production" << std::endl;
    co_return nil();
}

// 消费者协程 - 多次从队列取数据
Coroutine<nil> consumer(CoSchedulerHandle handle, std::shared_ptr<AsyncQueue<int, CommonError>> queue, int consumerId, int itemCount) {
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    std::cout << "[Consumer " << consumerId << "] Starting consumption" << std::endl;

    for (int i = 0; i < itemCount; ++i) {
        std::cout << "[Consumer " << consumerId << "] Round " << i << ": Waiting for item..." << std::endl;

        auto result = co_await queue->waitDequeue();
        if (!result) {
            std::cout << "[Consumer " << consumerId << "] Round " << i << ": Error: "
                      << result.error().message() << std::endl;
            continue;
        }

        std::cout << "[Consumer " << consumerId << "] Round " << i << ": Consumed item: "
                  << result.value() << ", queue size: " << queue->size() << std::endl;

        // 模拟消费过程
        co_await generator.sleep(200ms);
    }

    std::cout << "[Consumer " << consumerId << "] Finished consumption" << std::endl;
    co_return nil();
}

// 测试单生产者单消费者
Coroutine<nil> test_single_producer_consumer(CoSchedulerHandle handle) {
    std::cout << "\n=== Test Single Producer Single Consumer ===" << std::endl;

    auto queue = std::make_shared<AsyncQueue<int, CommonError>>();

    // 启动生产者和消费者
    handle.spawn(producer(handle, queue, 1, 5));
    handle.spawn(consumer(handle, queue, 1, 5));

    std::cout << "=== Test Single Producer Single Consumer End ===\n" << std::endl;
    co_return nil();
}

// 测试多生产者单消费者
Coroutine<nil> test_multiple_producers_single_consumer(CoSchedulerHandle handle) {
    std::cout << "\n=== Test Multiple Producers Single Consumer ===" << std::endl;

    auto queue = std::make_shared<AsyncQueue<int, CommonError>>();

    // 启动3个生产者，每个生产3个数据
    handle.spawn(producer(handle, queue, 1, 3));
    handle.spawn(producer(handle, queue, 2, 3));
    handle.spawn(producer(handle, queue, 3, 3));

    // 只启动1个消费者，消费所有9个数据
    handle.spawn(consumer(handle, queue, 1, 9));

    std::cout << "=== Test Multiple Producers Single Consumer End ===\n" << std::endl;
    co_return nil();
}

// 测试先push后wait的情况
Coroutine<nil> test_push_before_wait(CoSchedulerHandle handle) {
    std::cout << "\n=== Test Push Before Wait ===" << std::endl;

    auto queue = std::make_shared<AsyncQueue<int, CommonError>>();

    // 先push一些数据
    std::cout << "[Test] Pushing items before any wait..." << std::endl;
    queue->push(100);
    queue->push(200);
    queue->push(300);
    std::cout << "[Test] Queue size after push: " << queue->size() << std::endl;

    // 然后启动消费者
    handle.spawn(consumer(handle, queue, 1, 3));

    std::cout << "=== Test Push Before Wait End ===\n" << std::endl;
    co_return nil();
}

// 测试快速生产慢速消费
Coroutine<nil> fast_producer(CoSchedulerHandle handle, std::shared_ptr<AsyncQueue<int, CommonError>> queue) {
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    std::cout << "[Fast Producer] Starting..." << std::endl;

    for (int i = 0; i < 10; ++i) {
        co_await generator.sleep(50ms);
        std::cout << "[Fast Producer] Pushing: " << i << std::endl;
        queue->push(i);
    }

    std::cout << "[Fast Producer] Finished, final queue size: " << queue->size() << std::endl;
    co_return nil();
}

Coroutine<nil> slow_consumer(CoSchedulerHandle handle, std::shared_ptr<AsyncQueue<int, CommonError>> queue) {
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    std::cout << "[Slow Consumer] Starting..." << std::endl;

    for (int i = 0; i < 10; ++i) {
        auto result = co_await queue->waitDequeue();
        if (result) {
            std::cout << "[Slow Consumer] Got: " << result.value()
                      << ", queue size: " << queue->size() << std::endl;
        }
        co_await generator.sleep(500ms);
    }

    std::cout << "[Slow Consumer] Finished" << std::endl;
    co_return nil();
}

Coroutine<nil> test_fast_producer_slow_consumer(CoSchedulerHandle handle) {
    std::cout << "\n=== Test Fast Producer Slow Consumer ===" << std::endl;

    auto queue = std::make_shared<AsyncQueue<int, CommonError>>();

    handle.spawn(fast_producer(handle, queue));
    handle.spawn(slow_consumer(handle, queue));

    std::cout << "=== Test Fast Producer Slow Consumer End ===\n" << std::endl;
    co_return nil();
}

// 测试慢生产快消费（消费者会等待）
Coroutine<nil> slow_producer(CoSchedulerHandle handle, std::shared_ptr<AsyncQueue<int, CommonError>> queue) {
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    std::cout << "[Slow Producer] Starting..." << std::endl;

    for (int i = 0; i < 5; ++i) {
        co_await generator.sleep(800ms);
        std::cout << "[Slow Producer] Pushing: " << i << std::endl;
        queue->push(i);
    }

    std::cout << "[Slow Producer] Finished" << std::endl;
    co_return nil();
}

Coroutine<nil> fast_consumer(CoSchedulerHandle handle, std::shared_ptr<AsyncQueue<int, CommonError>> queue) {
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    std::cout << "[Fast Consumer] Starting..." << std::endl;

    for (int i = 0; i < 5; ++i) {
        std::cout << "[Fast Consumer] Waiting for item " << i << "..." << std::endl;
        auto result = co_await queue->waitDequeue();
        if (result) {
            std::cout << "[Fast Consumer] Got: " << result.value() << std::endl;
        }
        co_await generator.sleep(100ms);
    }

    std::cout << "[Fast Consumer] Finished" << std::endl;
    co_return nil();
}

Coroutine<nil> test_slow_producer_fast_consumer(CoSchedulerHandle handle) {
    std::cout << "\n=== Test Slow Producer Fast Consumer ===" << std::endl;

    auto queue = std::make_shared<AsyncQueue<int, CommonError>>();

    handle.spawn(slow_producer(handle, queue));
    handle.spawn(fast_consumer(handle, queue));

    std::cout << "=== Test Slow Producer Fast Consumer End ===\n" << std::endl;
    co_return nil();
}

int main() {
    RuntimeBuilder builder;
    Runtime runtime = builder.startCoManager(std::chrono::milliseconds(1000)).build();
    runtime.start();

    std::cout << "Starting AsyncQueue tests...\n" << std::endl;

    auto scheduler = runtime.getCoSchedulerHandle(0);
    if (!scheduler) {
        std::cerr << "Failed to get scheduler handle" << std::endl;
        return 1;
    }

    // 测试单生产者单消费者
    runtime.schedule(test_single_producer_consumer(*scheduler));
    getchar(); // 等待用户按键

    // 测试多生产者单消费者
    runtime.schedule(test_multiple_producers_single_consumer(*scheduler));
    getchar(); // 等待用户按键

    // 测试先push后wait
    runtime.schedule(test_push_before_wait(*scheduler));
    getchar(); // 等待用户按键

    // 测试快速生产慢速消费
    runtime.schedule(test_fast_producer_slow_consumer(*scheduler));
    getchar(); // 等待用户按键

    // 测试慢生产快消费
    runtime.schedule(test_slow_producer_fast_consumer(*scheduler));
    getchar(); // 等待用户按键

    std::cout << "All tests completed!" << std::endl;
    runtime.stop();
    return 0;
}