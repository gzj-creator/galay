/**
 * @file t97_ringfb.cc
 * @brief 验证 load ring 满时 injected 任务的搬运逻辑
 *
 * 场景：ring 只剩少量容量，inject_queue 中还有额外任务。调用 drainInjected()
 * 应该只搬运剩余容量数量的任务，剩余任务保持在 inject_queue，后续再次 drain
 * 能继续取到它们。
 */

#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>

#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> emptyTask() {
    co_return;
}

TaskRef makeTaskRef() {
    return detail::TaskAccess::detachTask(emptyTask());
}

bool runScenario() {
    constexpr size_t kRemainingCapacity = 4;
    constexpr size_t kExtraInjected = 3;
    constexpr size_t kRingCapacity = ChaseLevTaskRing::kCapacity;

    IOSchedulerWorkerState single_item_worker;
    if (!single_item_worker.local_ring.push_back(makeTaskRef())) {
        std::cerr << "[T97] failed to enqueue single-item ring probe\n";
        return false;
    }
    TaskRef single_popped;
    if (!single_item_worker.local_ring.pop_back(single_popped)) {
        std::cerr << "[T97] failed to pop single-item ring probe\n";
        return false;
    }
    if (!single_item_worker.local_ring.empty()) {
        std::cerr << "[T97] ring should be empty after single-item pop_back\n";
        return false;
    }

    IOSchedulerWorkerState worker;
    worker.resizeInjectBuffer(8);

    const size_t fill_count = kRingCapacity - kRemainingCapacity;
    for (size_t i = 0; i < fill_count; ++i) {
        if (!worker.local_ring.push_back(makeTaskRef())) {
            std::cerr << "[T97] failed to fill ring (index=" << i << ")\n";
            return false;
        }
    }

    const size_t total_injected = kRemainingCapacity + kExtraInjected;
    for (size_t i = 0; i < total_injected; ++i) {
        worker.scheduleInjected(makeTaskRef());
    }

    if (worker.injected_outstanding.load(std::memory_order_acquire) != total_injected) {
        std::cerr << "[T97] expected injected_outstanding == " << total_injected
                  << ", actual=" << worker.injected_outstanding.load(std::memory_order_acquire)
                  << "\n";
        return false;
    }

    const size_t first_drain = worker.drainInjected();
    if (first_drain != kRemainingCapacity) {
        std::cerr << "[T97] first drain should move " << kRemainingCapacity
                  << " tasks, moved=" << first_drain << "\n";
        return false;
    }
    if (!worker.hasPendingInjected()) {
        std::cerr << "[T97] expected pending injected tasks after first drain\n";
        return false;
    }

    for (size_t i = 0; i < kRemainingCapacity; ++i) {
        TaskRef popped;
        if (!worker.local_ring.pop_back(popped)) {
            std::cerr << "[T97] failed to free ring slot " << i << "\n";
            return false;
        }
    }

    const size_t second_drain = worker.drainInjected();
    const size_t expected_second = total_injected - first_drain;
    if (second_drain != expected_second) {
        std::cerr << "[T97] second drain expected " << expected_second
                  << " tasks, moved=" << second_drain << "\n";
        return false;
    }
    if (worker.hasPendingInjected()) {
        std::cerr << "[T97] no pending injected tasks expected after second drain\n";
        return false;
    }

    if (worker.injected_outstanding.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T97] injected_outstanding should be 0 after draining\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runScenario()) {
        return 1;
    }
    std::cout << "T97-ioscheduler_inject_ring_fallback PASS\n";
    return 0;
}
