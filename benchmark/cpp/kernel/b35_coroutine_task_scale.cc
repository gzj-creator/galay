/**
 * @file b35_coroutine_task_scale.cc
 * @brief 可参数化的百万/千万级 C++ Task 生命周期与驻留压力。
 *
 * 用法：
 *   benchmark_kernel_coroutine_task_scale [count] [churn|live]
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-kernel/core/task.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using galay::kernel::Task;

namespace {

Task<void> completed_task() {
    co_return;
}

Task<void> suspended_task() {
    co_await std::suspend_always{};
}

bool run_churn(std::size_t count) {
    std::size_t completed = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        auto task = completed_task();
        if (!task.is_valid()) {
            std::cerr << "task_scale allocation failed at=" << i << '\n';
            return false;
        }
        auto* state = galay::kernel::detail::TaskAccess::task_ref(task).state();
        if (state == nullptr || state->m_handle == nullptr) {
            std::cerr << "task_scale missing frame handle at=" << i << '\n';
            return false;
        }
        state->m_handle.resume();
        if (!state->is_done()) {
            std::cerr << "task_scale completion failed at=" << i << '\n';
            return false;
        }
        ++completed;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "task_scale mode=churn count=" << count
              << " completed=" << completed
              << " seconds=" << seconds
              << " tasks_per_sec="
              << (seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0)
              << '\n';
    return completed == count;
}

bool run_live(std::size_t count) {
    std::vector<Task<void>> tasks;
    tasks.reserve(count);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        auto task = suspended_task();
        if (!task.is_valid()) {
            std::cerr << "task_scale allocation failed at=" << i << '\n';
            return false;
        }
        tasks.push_back(std::move(task));
    }
    const double createSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    std::cout << "task_scale mode=live count=" << tasks.size()
              << " create_seconds=" << createSeconds
              << " tasks_per_sec="
              << (createSeconds > 0.0
                      ? static_cast<double>(tasks.size()) / createSeconds
                      : 0.0)
              << '\n';
    tasks.clear();
    std::cout << "task_scale mode=live_after_clear count=0\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    const std::size_t count = argc > 1
        ? std::strtoull(argv[1], nullptr, 10)
        : 1'000'000;
    const std::string_view mode = argc > 2 ? argv[2] : "churn";
    if (count == 0) {
        std::cerr << "task_scale count must be positive\n";
        return 1;
    }
    if (mode == "churn") {
        return run_churn(count) ? 0 : 1;
    }
    if (mode == "live") {
        return run_live(count) ? 0 : 1;
    }
    std::cerr << "task_scale mode must be churn or live\n";
    return 1;
}
