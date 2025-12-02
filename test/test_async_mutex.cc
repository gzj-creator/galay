#include "galay/kernel/coroutine/AsyncMutex.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/async/AsyncFactory.h"
#include <iostream>
#include <chrono>
#include <atomic>

using namespace galay;
using namespace std::chrono_literals;

// 全局共享状态
struct SharedState {
    int counter = 0;
    AsyncMutex mutex;
    std::atomic<int> errors = 0;
};

// ============================================================================
// 测试1: 单个 handle 内多个协程竞争（大数据量）
// ============================================================================
Coroutine<nil> single_handle_worker(std::shared_ptr<SharedState> state, int id, int iterations) {
    for (int i = 0; i < iterations; i++) {
        co_await state->mutex.lock();
        {
            int old_val = state->counter;
            // 模拟一些工作，增加竞争窗口
            state->counter = old_val + 1;

            if (state->counter != old_val + 1) {
                state->errors++;
            }
        }
        state->mutex.unlock();
    }
    co_return nil{};
}

Coroutine<nil> test_single_handle_contention(CoSchedulerHandle handle) {
    std::cout << "\n====== Test 1: Single Handle - Multiple Coroutines (High Volume) ======" << std::endl;

    auto state = std::make_shared<SharedState>();
    const int NUM_WORKERS = 20;        // 增加到 20 个协程
    const int ITERATIONS = 100;        // 每个 100 次操作
    const long long EXPECTED = (long long)NUM_WORKERS * ITERATIONS;

    std::cout << "Spawning " << NUM_WORKERS << " workers, " << ITERATIONS << " iterations each..." << std::endl;

    for (int i = 0; i < NUM_WORKERS; i++) {
        handle.spawn(single_handle_worker(state, i, ITERATIONS));
    }

    auto gen = handle.getAsyncFactory().getTimerGenerator();
    co_await gen.sleep(5000ms);

    std::cout << "Expected: " << EXPECTED << ", Actual: " << state->counter;
    if (state->counter == EXPECTED && state->errors == 0) {
        std::cout << " ✓ PASS" << std::endl;
    } else {
        std::cout << " ✗ FAIL (errors: " << state->errors << ")" << std::endl;
    }

    co_return nil{};
}

// ============================================================================
// 测试2: 极限高竞争场景
// ============================================================================
Coroutine<nil> intense_worker(std::shared_ptr<SharedState> state, int id) {
    for (int i = 0; i < 50; i++) {
        co_await state->mutex.lock();
        {
            state->counter++;
        }
        state->mutex.unlock();
    }
    co_return nil{};
}

Coroutine<nil> test_intense_contention(CoSchedulerHandle handle) {
    std::cout << "\n====== Test 2: Extreme Contention (50 workers) ======" << std::endl;

    auto state = std::make_shared<SharedState>();
    const int NUM_WORKERS = 50;        // 50 个协程
    const int EXPECTED = NUM_WORKERS * 50;

    std::cout << "Spawning " << NUM_WORKERS << " workers, 50 iterations each (total: " << EXPECTED << " ops)..." << std::endl;

    for (int i = 0; i < NUM_WORKERS; i++) {
        handle.spawn(intense_worker(state, i));
    }

    auto gen = handle.getAsyncFactory().getTimerGenerator();
    co_await gen.sleep(8000ms);

    std::cout << "Expected: " << EXPECTED << ", Actual: " << state->counter;
    if (state->counter == EXPECTED && state->errors == 0) {
        std::cout << " ✓ PASS" << std::endl;
    } else {
        std::cout << " ✗ FAIL" << std::endl;
    }

    co_return nil{};
}

// ============================================================================
// 测试3: 验证互斥性（大量检查）
// ============================================================================
Coroutine<nil> checker_worker(std::shared_ptr<SharedState> state, int id) {
    for (int i = 0; i < 50; i++) {
        co_await state->mutex.lock();
        {
            bool is_locked = state->mutex.isLocked();
            if (!is_locked) {
                std::cout << "[ERROR] Worker " << id << ": Lock not held!" << std::endl;
                state->errors++;
            }
            state->counter++;
        }
        state->mutex.unlock();
    }
    co_return nil{};
}

Coroutine<nil> test_mutex_semantics(CoSchedulerHandle handle) {
    std::cout << "\n====== Test 3: Mutex Semantics (30 workers × 50 ops) ======" << std::endl;

    auto state = std::make_shared<SharedState>();
    const int NUM_WORKERS = 30;        // 30 个协程
    const int EXPECTED = NUM_WORKERS * 50;

    std::cout << "Spawning " << NUM_WORKERS << " workers, 50 iterations each (total: " << EXPECTED << " ops)..." << std::endl;

    for (int i = 0; i < NUM_WORKERS; i++) {
        handle.spawn(checker_worker(state, i));
    }

    auto gen = handle.getAsyncFactory().getTimerGenerator();
    co_await gen.sleep(7000ms);

    std::cout << "Expected: " << EXPECTED << ", Actual: " << state->counter;
    if (state->counter == EXPECTED && state->errors == 0) {
        std::cout << " ✓ PASS" << std::endl;
    } else {
        std::cout << " ✗ FAIL (errors: " << state->errors << ")" << std::endl;
    }

    co_return nil{};
}

// ============================================================================
// 主函数
// ============================================================================
int main() {
    RuntimeBuilder builder;
    Runtime runtime = builder.startCoManager(std::chrono::milliseconds(1000)).build();
    runtime.start();

    std::cout << "\n╔══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║    AsyncMutex Test - High Volume & Race Detection   ║" << std::endl;
    std::cout << "║   Total Operations: 2000 + 2500 + 1500 = 6000      ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════╝" << std::endl;

    // 顺序运行三个测试，每个给足时间完成
    runtime.schedule(test_single_handle_contention(*runtime.getCoSchedulerHandle(0)));
    getchar();
    runtime.schedule(test_intense_contention(*runtime.getCoSchedulerHandle(1)));
    getchar();
    runtime.schedule(test_mutex_semantics(*runtime.getCoSchedulerHandle(2)));
    getchar();

    std::cout << "\n╔══════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                All tests completed!                  ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════╝\n" << std::endl;

    runtime.stop();
    return 0;
}
