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

#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <utility>

using namespace galay::kernel;

static_assert(noexcept(std::declval<Scheduler&>().scheduleResume(TaskRef{})),
              "waker resume admission must be noexcept");

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
        const auto admitted = worker.scheduleInjected(makeTaskRef());
        if (!admitted.has_value()) {
            std::cerr << "[T97] injected admission rejected task " << i << "\n";
            return false;
        }
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

bool runResumeAdmissionScenario() {
    constexpr size_t kResumeCount = 17;
    IOSchedulerWorkerState worker;

    for (size_t i = 0; i < kResumeCount; ++i) {
        const auto admitted = worker.scheduleResume(makeTaskRef());
        if (!admitted.has_value()) {
            std::cerr << "[T97] allocation-free resume admission rejected task "
                      << i << "\n";
            return false;
        }
    }

    if (!worker.hasPendingResume()) {
        std::cerr << "[T97] dedicated resume queue should contain admitted tasks\n";
        return false;
    }

    const size_t drained = worker.drainInjected();
    if (drained != kResumeCount) {
        std::cerr << "[T97] resume drain expected " << kResumeCount
                  << " tasks, moved=" << drained << "\n";
        return false;
    }

    size_t popped = 0;
    TaskRef task;
    while (worker.local_ring.pop_back(task)) {
        ++popped;
        task = TaskRef{};
    }
    if (popped != kResumeCount || worker.hasPendingInjected() ||
        worker.hasPendingResume()) {
        std::cerr << "[T97] resume admission drain mismatch, popped=" << popped
                  << "\n";
        return false;
    }
    return true;
}

bool runResumeFifoScenario() {
    IOSchedulerWorkerState worker;
    std::array<TaskRef, 3> tasks{
        makeTaskRef(),
        makeTaskRef(),
        makeTaskRef(),
    };
    const std::array<TaskState*, 3> expected{
        tasks[0].state(),
        tasks[1].state(),
        tasks[2].state(),
    };

    for (const TaskRef& task : tasks) {
        if (!worker.scheduleResume(task).has_value()) {
            std::cerr << "[T97] FIFO probe failed to admit resume task\n";
            return false;
        }
    }
    if (worker.drainInjected() != tasks.size()) {
        std::cerr << "[T97] FIFO probe failed to drain resume tasks\n";
        return false;
    }

    for (size_t i = 0; i < expected.size(); ++i) {
        TaskRef popped;
        if (!worker.local_ring.pop_back(popped) ||
            popped.state() != expected[i]) {
            std::cerr << "[T97] resume order mismatch at index " << i << "\n";
            return false;
        }
        if (!detail::resumeTaskState(popped.state())) {
            std::cerr << "[T97] FIFO probe failed to resume task " << i << "\n";
            return false;
        }
    }
    return true;
}

bool runResumeAdmissionLifecycleScenario() {
    IOSchedulerWorkerState worker;
    worker.closeResumeAdmission();
    if (worker.scheduleResume(makeTaskRef()).has_value()) {
        std::cerr << "[T97] closed worker accepted a resume task\n";
        return false;
    }
    if (!worker.reopenResumeAdmission()) {
        std::cerr << "[T97] empty worker failed to reopen resume admission\n";
        return false;
    }

    const auto admitted = worker.scheduleResume(makeTaskRef());
    if (!admitted.has_value()) {
        std::cerr << "[T97] reopened worker rejected a resume task\n";
        return false;
    }
    worker.closeResumeAdmission();
    if (worker.scheduleResume(makeTaskRef()).has_value()) {
        std::cerr << "[T97] stopped worker accepted a new resume task\n";
        return false;
    }
    if (worker.drainInjected() != 1) {
        std::cerr << "[T97] stopped worker failed to drain accepted resume task\n";
        return false;
    }

    TaskRef task;
    if (!worker.local_ring.pop_back(task) || !task.isValid() ||
        !detail::resumeTaskState(task.state())) {
        std::cerr << "[T97] stopped worker failed to run accepted resume task\n";
        return false;
    }
    if (!worker.reopenResumeAdmission()) {
        std::cerr << "[T97] drained worker failed to reopen resume admission\n";
        return false;
    }
    worker.closeResumeAdmission();
    return true;
}

bool runResumeFairnessScenario() {
    constexpr size_t kRingCapacity = ChaseLevTaskRing::kCapacity;
    constexpr size_t kAttempts = 8;
    IOSchedulerWorkerState worker;
    worker.resizeInjectBuffer(8);

    for (size_t i = 0; i + 1 < kRingCapacity; ++i) {
        if (!worker.local_ring.push_back(makeTaskRef())) {
            std::cerr << "[T97] failed to fill fairness ring at " << i << "\n";
            return false;
        }
    }

    TaskRef resume_task = makeTaskRef();
    const auto resume_admitted = worker.scheduleResume(resume_task);
    if (!resume_admitted.has_value()) {
        std::cerr << "[T97] fairness resume admission failed\n";
        return false;
    }

    bool observed_resume = false;
    for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
        const auto normal_admitted = worker.scheduleInjected(makeTaskRef());
        if (!normal_admitted.has_value()) {
            std::cerr << "[T97] fairness normal admission failed\n";
            return false;
        }
        if (worker.drainInjected() != 1) {
            std::cerr << "[T97] fairness drain should consume one ring slot\n";
            return false;
        }
        TaskRef popped;
        if (!worker.local_ring.pop_back(popped) || !popped.isValid()) {
            std::cerr << "[T97] fairness probe failed to pop drained task\n";
            return false;
        }
        if (popped.state()->m_resume_queue_claimed.load(
                std::memory_order_acquire)) {
            observed_resume = true;
            if (!detail::resumeTaskState(popped.state())) {
                std::cerr << "[T97] fairness probe failed to resume admitted task\n";
                return false;
            }
            break;
        }
    }

    if (!observed_resume) {
        std::cerr << "[T97] sustained normal injection starved resume admission\n";
        return false;
    }
    return true;
}

bool runResumeQueueCloseScenario() {
    constexpr size_t kProducerCount = 4;
    constexpr size_t kTasksPerProducer = 256;

    detail::TaskResumeQueue queue;
    TaskRef accepted_before_close = makeTaskRef();
    if (!queue.push(accepted_before_close)) {
        std::cerr << "[T97] open resume queue rejected pre-close task\n";
        return false;
    }

    std::atomic<bool> start{false};
    std::array<size_t, kProducerCount> accepted{};
    std::array<std::thread, kProducerCount> producers;
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers[producer] = std::thread([&, producer]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            size_t local_accepted = 0;
            for (size_t i = 0; i < kTasksPerProducer; ++i) {
                if (queue.push(makeTaskRef())) {
                    ++local_accepted;
                }
            }
            accepted[producer] = local_accepted;
        });
    }
    std::thread closer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        queue.close();
    });

    start.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        if (!producer.joinable()) {
            std::cerr << "[T97] resume queue producer thread was not joinable\n";
            closer.join();
            return false;
        }
        producer.join();
    }
    if (!closer.joinable()) {
        std::cerr << "[T97] resume queue closer thread was not joinable\n";
        return false;
    }
    closer.join();

    if (!queue.isClosed()) {
        std::cerr << "[T97] close should reject subsequent resume admission\n";
        return false;
    }
    if (queue.push(makeTaskRef())) {
        std::cerr << "[T97] closed resume queue accepted a task\n";
        return false;
    }

    size_t expected = 1;
    for (const size_t count : accepted) {
        expected += count;
    }
    size_t drained = 0;
    TaskState* ready = detail::TaskResumeQueue::reverse(queue.takeAll());
    while (ready != nullptr) {
        TaskRef task = detail::TaskResumeQueue::popFront(ready);
        if (!task.isValid()) {
            std::cerr << "[T97] close race produced an invalid resume node\n";
            return false;
        }
        ++drained;
    }
    if (drained != expected || !queue.empty() || !queue.isClosed()) {
        std::cerr << "[T97] close race lost accepted tasks, expected=" << expected
                  << ", drained=" << drained << "\n";
        return false;
    }

    if (!queue.reopen() || queue.isClosed() || !queue.push(makeTaskRef())) {
        std::cerr << "[T97] reopened resume queue rejected admission\n";
        return false;
    }
    detail::TaskResumeQueue::releaseAll(queue.takeAll());
    return true;
}

bool runResumeQueueDuplicateScenario() {
    detail::TaskResumeQueue queue;
    TaskRef task = makeTaskRef();
    if (!queue.push(task)) {
        std::cerr << "[T97] resume queue rejected first task reference\n";
        return false;
    }
    if (queue.push(task)) {
        std::cerr << "[T97] resume queue accepted duplicate task reference\n";
        return false;
    }

    TaskState* ready = detail::TaskResumeQueue::reverse(queue.takeAll());
    TaskRef admitted = detail::TaskResumeQueue::popFront(ready);
    if (!admitted.isValid() || ready != nullptr) {
        std::cerr << "[T97] duplicate probe should drain exactly one task\n";
        return false;
    }
    if (queue.push(task)) {
        std::cerr << "[T97] detached task was re-admitted before resume\n";
        return false;
    }
    if (!detail::resumeTaskState(admitted.state())) {
        std::cerr << "[T97] failed to resume duplicate-probe task\n";
        return false;
    }
    if (!queue.push(task)) {
        std::cerr << "[T97] resumed task did not release resume queue claim\n";
        return false;
    }
    detail::TaskResumeQueue::releaseAll(queue.takeAll());

    TaskRef released_task = makeTaskRef();
    if (!queue.push(released_task)) {
        std::cerr << "[T97] resume queue rejected release-probe task\n";
        return false;
    }
    TaskState* released_ready = queue.takeAll();
    if (queue.push(released_task)) {
        std::cerr << "[T97] detached task was re-admitted before release\n";
        detail::TaskResumeQueue::releaseAll(released_ready);
        return false;
    }
    detail::TaskResumeQueue::releaseAll(released_ready);
    if (!queue.push(released_task)) {
        std::cerr << "[T97] released task did not release resume queue claim\n";
        return false;
    }
    detail::TaskResumeQueue::releaseAll(queue.takeAll());
    return true;
}

}  // namespace

int main() {
    if (!runScenario()) {
        return 1;
    }
    if (!runResumeAdmissionScenario()) {
        return 1;
    }
    if (!runResumeFifoScenario()) {
        return 1;
    }
    if (!runResumeAdmissionLifecycleScenario()) {
        return 1;
    }
    if (!runResumeFairnessScenario()) {
        return 1;
    }
    if (!runResumeQueueCloseScenario()) {
        return 1;
    }
    if (!runResumeQueueDuplicateScenario()) {
        return 1;
    }
    std::cout << "T97-ioscheduler_inject_ring_fallback PASS\n";
    return 0;
}
