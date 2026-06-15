/**
 * @file t54_then.cc
 * @brief 用途：验证 `Task<void>::then(...)` 链式 continuation 在当前实现下可正确工作。
 * 关键覆盖点：`Task<void>` continuation 绑定、链式调度顺序、完成通知与执行收尾。
 * 通过条件：链式 continuation 成功执行且顺序符合预期，测试返回 0。
 */

#include "galay-kernel/core/compute_scheduler.h"
#include "galay-kernel/core/task.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::kernel;

namespace {

std::mutex g_sequence_mutex;
std::vector<int> g_sequence;
std::atomic<int> g_completed{0};

Task<void> pushStep(int step) {
    {
        std::lock_guard<std::mutex> lock(g_sequence_mutex);
        g_sequence.push_back(step);
    }
    g_completed.fetch_add(1, std::memory_order_release);
    co_return;
}

}  // namespace

static_assert(std::is_same_v<decltype(std::declval<Task<void>&>().then(pushStep(2))), Task<void>&>);
static_assert(std::is_same_v<decltype(std::declval<Task<void>>().then(pushStep(2))), Task<void>&&>);

int main() {
    ComputeScheduler scheduler;
    scheduler.start();

    scheduler.schedule(detail::TaskAccess::detachTask(pushStep(1).then(pushStep(2))));

    for (int i = 0; i < 100 && g_completed.load(std::memory_order_acquire) < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    scheduler.stop();

    assert(g_completed.load(std::memory_order_acquire) == 2);
    assert((g_sequence == std::vector<int>{1, 2}));

    std::cout << "T54-TaskThenCompat PASS\n";
    return 0;
}
