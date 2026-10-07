/**
 * @file t97_ringfb.cc
 * @brief 验证 load ring 满时 injected 任务的搬运逻辑
 *
 * 场景：ring 只剩少量容量，inject_queue 中还有额外任务。调用 drain_injected()
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

static_assert(noexcept(std::declval<Scheduler&>().schedule_resume(TaskRef{})),
              "waker resume admission must be noexcept");

namespace {

Task<void> empty_task() {
    co_return;
}

TaskRef make_task_ref() {
    return detail::TaskAccess::detach_task(empty_task());
}

bool run_scenario() {
    constexpr size_t kRemainingCapacity = 4;
    constexpr size_t kExtraInjected = 3;
    constexpr size_t kRingCapacity = ChaseLevTaskRing::kCapacity;

    IOReadyQueue single_item_worker;
    if (!single_item_worker.local_ring.push_back(make_task_ref())) {
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

    IOReadyQueue worker;
    worker.resize_inject_buffer(8);

    const size_t fill_count = kRingCapacity - kRemainingCapacity;
    for (size_t i = 0; i < fill_count; ++i) {
        if (!worker.local_ring.push_back(make_task_ref())) {
            std::cerr << "[T97] failed to fill ring (index=" << i << ")\n";
            return false;
        }
    }

    const size_t total_injected = kRemainingCapacity + kExtraInjected;
    for (size_t i = 0; i < total_injected; ++i) {
        const auto admitted = worker.schedule_injected(make_task_ref());
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

    const size_t first_drain = worker.drain_injected();
    if (first_drain != kRemainingCapacity) {
        std::cerr << "[T97] first drain should move " << kRemainingCapacity
                  << " tasks, moved=" << first_drain << "\n";
        return false;
    }
    if (!worker.has_pending_injected()) {
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

    const size_t second_drain = worker.drain_injected();
    const size_t expected_second = total_injected - first_drain;
    if (second_drain != expected_second) {
        std::cerr << "[T97] second drain expected " << expected_second
                  << " tasks, moved=" << second_drain << "\n";
        return false;
    }
    if (worker.has_pending_injected()) {
        std::cerr << "[T97] no pending injected tasks expected after second drain\n";
        return false;
    }

    if (worker.injected_outstanding.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T97] injected_outstanding should be 0 after draining\n";
        return false;
    }

    return true;
}

bool run_resume_admission_scenario() {
    constexpr size_t kResumeCount = 17;
    IOReadyQueue worker;

    for (size_t i = 0; i < kResumeCount; ++i) {
        const auto admitted = worker.schedule_resume(make_task_ref());
        if (!admitted.has_value()) {
            std::cerr << "[T97] allocation-free resume admission rejected task "
                      << i << "\n";
            return false;
        }
    }

    if (!worker.has_pending_resume()) {
        std::cerr << "[T97] dedicated resume queue should contain admitted tasks\n";
        return false;
    }

    const size_t drained = worker.drain_injected();
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
    if (popped != kResumeCount || worker.has_pending_injected() ||
        worker.has_pending_resume()) {
        std::cerr << "[T97] resume admission drain mismatch, popped=" << popped
                  << "\n";
        return false;
    }
    return true;
}

bool run_resume_fifo_scenario() {
    IOReadyQueue worker;
    std::array<TaskRef, 3> tasks{
        make_task_ref(),
        make_task_ref(),
        make_task_ref(),
    };
    const std::array<TaskState*, 3> expected{
        tasks[0].state(),
        tasks[1].state(),
        tasks[2].state(),
    };

    for (const TaskRef& task : tasks) {
        if (!worker.schedule_resume(task).has_value()) {
            std::cerr << "[T97] FIFO probe failed to admit resume task\n";
            return false;
        }
    }
    if (worker.drain_injected() != tasks.size()) {
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
        if (!detail::resume_task_state(popped.state())) {
            std::cerr << "[T97] FIFO probe failed to resume task " << i << "\n";
            return false;
        }
    }
    return true;
}

bool run_resume_batched_fifo_scenario(bool stealing_enabled) {
    constexpr size_t capacity = ChaseLevTaskRing::kCapacity;
    std::array<TaskRef, capacity + 17> tasks;
    IOReadyQueue worker;
    worker.set_stealing_enabled(stealing_enabled);
    for (TaskRef& task : tasks) {
        task = make_task_ref();
        if (!worker.schedule_resume(task).has_value()) { return false; }
    }
    size_t offset = 0;
    while (offset < tasks.size()) {
        const size_t remaining = tasks.size() - offset;
        const size_t expected = remaining < capacity ? remaining : capacity;
        if (worker.drain_injected() != expected) {
            std::cerr << "[T97] partial resume batch size mismatch\n";
            return false;
        }
        for (size_t i = 0; i < expected; ++i) {
            TaskRef popped;
            if (!worker.local_ring.pop_back(popped) ||
                popped.state() != tasks[offset + i].state() ||
                !detail::resume_task_state(popped.state())) {
                std::cerr << "[T97] partial resume batch FIFO mismatch\n";
                return false;
            }
        }
        offset += expected;
        if (!worker.local_ring.empty() ||
            worker.injected_outstanding.load() != tasks.size() - offset) { return false; }
    }
    for (const TaskRef& task : tasks) {
        if (task.state()->m_refs.load() != 1 ||
            task.state()->m_resume_queue_claimed.load()) { return false; }
    }
    return !worker.has_pending_resume() && !worker.has_pending_injected();
}

bool run_resume_admission_lifecycle_scenario() {
    IOReadyQueue worker;
    worker.close_resume_admission();
    if (worker.schedule_resume(make_task_ref()).has_value()) {
        std::cerr << "[T97] closed worker accepted a resume task\n";
        return false;
    }
    if (!worker.reopen_resume_admission()) {
        std::cerr << "[T97] empty worker failed to reopen resume admission\n";
        return false;
    }

    const auto admitted = worker.schedule_resume(make_task_ref());
    if (!admitted.has_value()) {
        std::cerr << "[T97] reopened worker rejected a resume task\n";
        return false;
    }
    worker.close_resume_admission();
    if (worker.schedule_resume(make_task_ref()).has_value()) {
        std::cerr << "[T97] stopped worker accepted a new resume task\n";
        return false;
    }
    if (worker.drain_injected() != 1) {
        std::cerr << "[T97] stopped worker failed to drain accepted resume task\n";
        return false;
    }

    TaskRef task;
    if (!worker.local_ring.pop_back(task) || !task.is_valid() ||
        !detail::resume_task_state(task.state())) {
        std::cerr << "[T97] stopped worker failed to run accepted resume task\n";
        return false;
    }
    if (!worker.reopen_resume_admission()) {
        std::cerr << "[T97] drained worker failed to reopen resume admission\n";
        return false;
    }
    worker.close_resume_admission();
    return true;
}

bool run_resume_fairness_scenario() {
    constexpr size_t kRingCapacity = ChaseLevTaskRing::kCapacity;
    constexpr size_t kAttempts = 8;
    IOReadyQueue worker;
    worker.resize_inject_buffer(8);

    for (size_t i = 0; i + 1 < kRingCapacity; ++i) {
        if (!worker.local_ring.push_back(make_task_ref())) {
            std::cerr << "[T97] failed to fill fairness ring at " << i << "\n";
            return false;
        }
    }

    TaskRef resume_task = make_task_ref();
    const auto resume_admitted = worker.schedule_resume(resume_task);
    if (!resume_admitted.has_value()) {
        std::cerr << "[T97] fairness resume admission failed\n";
        return false;
    }

    bool observed_resume = false;
    for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
        const auto normal_admitted = worker.schedule_injected(make_task_ref());
        if (!normal_admitted.has_value()) {
            std::cerr << "[T97] fairness normal admission failed\n";
            return false;
        }
        if (worker.drain_injected() != 1) {
            std::cerr << "[T97] fairness drain should consume one ring slot\n";
            return false;
        }
        TaskRef popped;
        if (!worker.local_ring.pop_back(popped) || !popped.is_valid()) {
            std::cerr << "[T97] fairness probe failed to pop drained task\n";
            return false;
        }
        if (popped.state()->m_resume_queue_claimed.load(
                std::memory_order_acquire)) {
            observed_resume = true;
            if (!detail::resume_task_state(popped.state())) {
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

bool run_resume_queue_close_scenario() {
    constexpr size_t kProducerCount = 4;
    constexpr size_t kTasksPerProducer = 256;

    detail::TaskResumeQueue queue;
    TaskRef accepted_before_close = make_task_ref();
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
                if (queue.push(make_task_ref())) {
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

    if (!queue.is_closed()) {
        std::cerr << "[T97] close should reject subsequent resume admission\n";
        return false;
    }
    if (queue.push(make_task_ref())) {
        std::cerr << "[T97] closed resume queue accepted a task\n";
        return false;
    }

    size_t expected = 1;
    for (const size_t count : accepted) {
        expected += count;
    }
    size_t drained = 0;
    TaskState* ready = detail::TaskResumeQueue::reverse(queue.take_all());
    while (ready != nullptr) {
        TaskRef task = detail::TaskResumeQueue::pop_front(ready);
        if (!task.is_valid()) {
            std::cerr << "[T97] close race produced an invalid resume node\n";
            return false;
        }
        ++drained;
    }
    if (drained != expected || !queue.empty() || !queue.is_closed()) {
        std::cerr << "[T97] close race lost accepted tasks, expected=" << expected
                  << ", drained=" << drained << "\n";
        return false;
    }

    if (!queue.reopen() || queue.is_closed() || !queue.push(make_task_ref())) {
        std::cerr << "[T97] reopened resume queue rejected admission\n";
        return false;
    }
    detail::TaskResumeQueue::release_all(queue.take_all());
    return true;
}

bool run_resume_queue_duplicate_scenario() {
    detail::TaskResumeQueue queue;
    TaskRef task = make_task_ref();
    if (!queue.push(task)) {
        std::cerr << "[T97] resume queue rejected first task reference\n";
        return false;
    }
    if (queue.push(task)) {
        std::cerr << "[T97] resume queue accepted duplicate task reference\n";
        return false;
    }

    TaskState* ready = detail::TaskResumeQueue::reverse(queue.take_all());
    TaskRef admitted = detail::TaskResumeQueue::pop_front(ready);
    if (!admitted.is_valid() || ready != nullptr) {
        std::cerr << "[T97] duplicate probe should drain exactly one task\n";
        return false;
    }
    if (queue.push(task)) {
        std::cerr << "[T97] detached task was re-admitted before resume\n";
        return false;
    }
    if (!detail::resume_task_state(admitted.state())) {
        std::cerr << "[T97] failed to resume duplicate-probe task\n";
        return false;
    }
    if (!queue.push(task)) {
        std::cerr << "[T97] resumed task did not release resume queue claim\n";
        return false;
    }
    detail::TaskResumeQueue::release_all(queue.take_all());

    TaskRef released_task = make_task_ref();
    if (!queue.push(released_task)) {
        std::cerr << "[T97] resume queue rejected release-probe task\n";
        return false;
    }
    TaskState* released_ready = queue.take_all();
    if (queue.push(released_task)) {
        std::cerr << "[T97] detached task was re-admitted before release\n";
        detail::TaskResumeQueue::release_all(released_ready);
        return false;
    }
    detail::TaskResumeQueue::release_all(released_ready);
    if (!queue.push(released_task)) {
        std::cerr << "[T97] released task did not release resume queue claim\n";
        return false;
    }
    detail::TaskResumeQueue::release_all(queue.take_all());
    return true;
}

bool run_resume_queue_ownership_scenario() {
    static_assert(alignof(TaskState) >= 2); // The low bit belongs to the promise view.
    detail::TaskResumeQueue queue;
    if (queue.push(TaskRef{})) {
        std::cerr << "[T97] empty reference was admitted\n";
        return false;
    }
    TaskRef owner = make_task_ref();
    TaskState* state = owner.state();
    const auto handle = std::coroutine_handle<TaskPromise<void>>::from_address(
        state->m_handle.address());
    const TaskRef& borrowed = handle.promise().task_ref_view();
    if (!borrowed.is_valid() || borrowed.state() != state ||
        (reinterpret_cast<uintptr_t>(state) & 1U) != 0 || state->m_refs.load() != 1) {
        std::cerr << "[T97] invalid aligned promise view\n";
        return false;
    }
    // Passing the const borrowed view by value must materialize an owning ref.
    if (!queue.push(borrowed) || state->m_refs.load() != 2 ||
        !borrowed.is_valid() || queue.push(borrowed) || state->m_refs.load() != 2) {
        std::cerr << "[T97] borrowed view copy/duplicate ownership imbalance\n";
        return false;
    }
    detail::TaskResumeQueue::release_all(queue.take_all());
    if (state->m_refs.load() != 1 || state->m_resume_queue_claimed.load()) {
        std::cerr << "[T97] drain did not release reference and claim\n";
        return false;
    }
    TaskRef moved(borrowed);
    if (!queue.push(std::move(moved)) || moved.is_valid() ||
        queue.push(std::move(moved)) || state->m_refs.load() != 2) {
        std::cerr << "[T97] moved reference admission imbalance\n";
        return false;
    }
    detail::TaskResumeQueue::release_all(queue.take_all());
    queue.close();
    if (queue.push(borrowed) || state->m_refs.load() != 1 ||
        state->m_resume_queue_claimed.load() || !queue.empty()) {
        std::cerr << "[T97] closed rejection did not restore ownership\n";
        return false;
    }
    std::cout << "T97 resume ownership: empty/borrowed-copy/move/duplicate/drain/close PASS\n";
    return true;
}


bool run_mixed_drain_accounting_scenario() {
    for (bool stealing : {false, true}) {
        IOReadyQueue worker;
        worker.set_stealing_enabled(stealing);
        worker.resize_inject_buffer(8);
        std::array<TaskRef, 32> normal;
        std::array<TaskRef, 17> resumes;
        for (auto& task : normal) { task = make_task_ref(); }
        for (auto& task : resumes) { task = make_task_ref(); }
        for (size_t i = 0; i < normal.size(); ++i) {
            const auto accepted = worker.schedule_injected(normal[i]);
            if (!accepted.has_value() || *accepted != (i == 0)) { return false; }
        }
        for (auto& task : resumes) {
            const auto accepted = worker.schedule_resume(task);
            if (!accepted.has_value() || *accepted) { return false; }
        }
        constexpr size_t kSlots = 7;
        for (size_t i = kSlots; i < ChaseLevTaskRing::kCapacity; ++i) {
            if (!worker.local_ring.push_back(make_task_ref())) { return false; }
        }
        size_t normal_seen = 0;
        size_t resume_seen = 0;
        while (worker.has_pending_injected()) {
            const auto before = worker.injected_outstanding.load();
            const size_t drained = worker.drain_injected();
            if (drained == 0 || drained > kSlots ||
                worker.injected_outstanding.load() != before - drained ||
                !worker.has_owner_drained_injected()) { return false; }
            for (size_t i = 0; i < drained; ++i) {
                TaskRef task;
                if (!worker.local_ring.pop_back(task)) { return false; }
                if (task.state()->m_resume_queue_claimed.load()) {
                    if (resume_seen >= resumes.size() ||
                        task.state() != resumes[resume_seen++].state() ||
                        !detail::resume_task_state(task.state())) { return false; }
                } else {
                    if (normal_seen >= normal.size() ||
                        task.state() != normal[normal_seen++].state()) { return false; }
                }
            }
        }
        if (normal_seen != normal.size() || resume_seen != resumes.size() ||
            worker.has_pending_resume() || worker.drain_injected() != 0) { return false; }
        for (auto& task : normal) {
            if (task.state()->m_refs.load() != 1) { return false; }
        }
        for (auto& task : resumes) {
            if (task.state()->m_refs.load() != 1 ||
                task.state()->m_resume_queue_claimed.load()) { return false; }
        }
        const auto next = worker.schedule_injected(make_task_ref());
        if (!next.has_value() || !*next || worker.drain_injected() != 1 ||
            worker.injected_outstanding.load() != 0) { return false; }
    }
    std::cout << "T97 mixed drain: bounded batches/FIFO/pending/zero transition PASS\n";
    return true;
}

bool run_mixed_drain_concurrent_scenario() {
    constexpr size_t kItems = 8192;
    for (bool close_during_drain : {false, true}) {
        IOReadyQueue worker;
        worker.set_stealing_enabled(false);
        worker.resize_inject_buffer(8);
        std::array<TaskRef, kItems> normal;
        std::array<TaskRef, kItems> resumes;
        for (auto& task : normal) { task = make_task_ref(); }
        for (auto& task : resumes) { task = make_task_ref(); }
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};
        std::atomic<size_t> progress{0};
        std::array<size_t, 2> accepted{};
        std::array<std::thread, 2> producers;
        for (size_t p = 0; p < producers.size(); ++p) {
            producers[p] = std::thread([&, p]() {
                while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
                for (size_t i = 0; i < kItems; ++i) {
                    const auto result = p == 0 ? worker.schedule_injected(normal[i])
                                               : worker.schedule_resume(resumes[i]);
                    accepted[p] += result.has_value() ? 1 : 0;
                    progress.fetch_add(1, std::memory_order_release);
                }
                done.fetch_add(1, std::memory_order_release);
            });
        }
        std::array<size_t, 2> consumed{};
        bool valid = true;
        start.store(true, std::memory_order_release);
        do {
            if (close_during_drain && progress.load(std::memory_order_acquire) >= kItems) {
                worker.close_resume_admission();
            }
            const size_t drained = worker.drain_injected();
            size_t popped = 0;
            TaskRef task;
            while (worker.local_ring.pop_back(task)) {
                const bool resume = task.state()->m_resume_queue_claimed.load();
                ++consumed[resume ? 1 : 0];
                if (resume && !detail::resume_task_state(task.state())) { valid = false; }
                task = TaskRef{};
                ++popped;
            }
            if (drained != popped || worker.injected_outstanding.load() > 2 * kItems) {
                valid = false;
            }
            std::this_thread::yield();
        } while (done.load(std::memory_order_acquire) != 2 || worker.has_pending_injected());
        for (auto& producer : producers) { producer.join(); }
        if (!valid || accepted[0] != kItems || accepted != consumed ||
            worker.injected_outstanding.load() != 0 || worker.has_pending_resume() ||
            (!close_during_drain && accepted[1] != kItems) ||
            !worker.reopen_resume_admission()) { return false; }
        for (auto& task : normal) {
            if (task.state()->m_refs.load() != 1) { return false; }
        }
        for (auto& task : resumes) {
            if (task.state()->m_refs.load() != 1 ||
                task.state()->m_resume_queue_claimed.load()) { return false; }
        }
    }
    std::cout << "T97 mixed drain: concurrent normal/resume/close/accounting PASS\n";
    return true;
}

bool run_resume_queue_cross_queue_claim_scenario() {
    constexpr size_t kProducers = 4;
    for (size_t round = 0; round < 64; ++round) {
        std::array<detail::TaskResumeQueue, 2> queues;
        TaskRef task = make_task_ref();
        std::array<TaskRef, 2> guards{make_task_ref(), make_task_ref()};
        for (size_t i = 0; i < queues.size(); ++i) {
            if (!queues[i].push(guards[i])) { return false; }
        }
        std::atomic<bool> start{false};
        std::array<size_t, kProducers> accepted{};
        std::array<std::thread, kProducers> producers;
        for (size_t p = 0; p < kProducers; ++p) {
            producers[p] = std::thread([&, p, owned = TaskRef(task)]() {
                while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
                for (size_t i = 0; i < 256; ++i) {
                    accepted[p] += queues[p % queues.size()].push(owned) ? 1 : 0;
                }
            });
        }
        start.store(true, std::memory_order_release);
        size_t total = 0;
        for (size_t p = 0; p < kProducers; ++p) {
            producers[p].join();
            total += accepted[p];
        }
        if (total != 1 || task.state()->m_refs.load() != 2 ||
            !task.state()->m_resume_queue_claimed.load()) { return false; }
        size_t seen = 0;
        for (size_t i = 0; i < queues.size(); ++i) {
            TaskState* ready = queues[i].take_all();
            const bool has_task = ready == task.state();
            TaskState* guard = has_task ? ready->m_resume_queue_next : ready;
            if (guard != guards[i].state() || guard->m_resume_queue_next != nullptr) {
                std::cerr << "[T97] competing queues corrupted intrusive links\n";
                return false;
            }
            seen += has_task ? 1 : 0;
            // Even a detached node still owns its claim in both queues.
            if (has_task && (queues[0].push(task) || queues[1].push(task))) { return false; }
            detail::TaskResumeQueue::release_all(ready);
        }
        if (seen != 1 || task.state()->m_refs.load() != 1 ||
            task.state()->m_resume_queue_claimed.load() || !queues[1].push(task)) {
            return false;
        }
        detail::TaskResumeQueue::release_all(queues[1].take_all());
    }
    std::cout << "T97 resume claim: same-state producers/cross-queue/detached/refs PASS\n";
    return true;
}

bool run_resume_queue_same_state_drain_scenario() {
    constexpr size_t kProducers = 4;
    constexpr size_t kAttempts = 20000;
    for (bool close_during_drain : {false, true}) {
        detail::TaskResumeQueue queue;
        TaskRef task = make_task_ref();
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};
        std::atomic<size_t> progress{0};
        std::array<size_t, kProducers> accepted{};
        std::array<std::thread, kProducers> producers;
        for (size_t p = 0; p < kProducers; ++p) {
            producers[p] = std::thread([&, p, owned = TaskRef(task)]() {
                while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
                for (size_t i = 0; i < kAttempts; ++i) {
                    accepted[p] += queue.push(owned) ? 1 : 0;
                    progress.fetch_add(1, std::memory_order_release);
                }
                done.fetch_add(1, std::memory_order_release);
            });
        }
        bool valid = true;
        size_t consumed = 0;
        start.store(true, std::memory_order_release);
        do {
            if (close_during_drain && progress.load(std::memory_order_acquire) >= kAttempts) {
                queue.close();
            }
            TaskState* ready = queue.take_all();
            if (ready != nullptr) {
                if (ready != task.state() || ready->m_resume_queue_next != nullptr ||
                    !ready->m_resume_queue_claimed.load()) { valid = false; }
                ++consumed;
                detail::TaskResumeQueue::release_all(ready);
            }
            std::this_thread::yield();
        } while (done.load(std::memory_order_acquire) != kProducers || !queue.empty());
        size_t total = 0;
        for (size_t p = 0; p < kProducers; ++p) {
            producers[p].join();
            total += accepted[p];
        }
        if (!valid || total != consumed || consumed == 0 || !queue.empty() ||
            task.state()->m_refs.load() != 1 || task.state()->m_resume_queue_claimed.load() ||
            queue.is_closed() != close_during_drain) {
            std::cerr << "[T97] same-state drain/close lost admission or ownership\n";
            return false;
        }
        if (!queue.reopen() || !queue.push(task)) { return false; }
        detail::TaskResumeQueue::release_all(queue.take_all());
    }
    std::cout << "T97 resume claim: same-state concurrent drain/close/reclaim PASS\n";
    return true;
}

bool run_resume_duplicate_pending_scenario() {
    IOReadyQueue worker;
    TaskRef task = make_task_ref();
    if (!worker.schedule_resume(task).has_value()) { return false; }
    for (size_t i = 0; i < 4096; ++i) {
        if (worker.schedule_resume(task).has_value() ||
            worker.injected_outstanding.load() != 1 || task.state()->m_refs.load() != 2) {
            return false;
        }
    }
    if (worker.drain_injected() != 1 || worker.injected_outstanding.load() != 0) { return false; }
    TaskRef popped;
    if (!worker.local_ring.pop_back(popped) || popped.state() != task.state() ||
        worker.schedule_resume(task).has_value() || worker.injected_outstanding.load() != 0 ||
        !detail::resume_task_state(popped.state())) { return false; }
    popped = TaskRef{};
    if (task.state()->m_refs.load() != 1 || task.state()->m_resume_queue_claimed.load() ||
        worker.has_pending_resume() || worker.has_pending_injected()) { return false; }
    worker.close_resume_admission();
    if (worker.schedule_resume(task).has_value() || worker.injected_outstanding.load() != 0 ||
        task.state()->m_refs.load() != 1 || task.state()->m_resume_queue_claimed.load()) {
        return false;
    }
    std::cout << "T97 resume claim: IO duplicate/pending rollback/drain/close PASS\n";
    return true;
}

bool run_resume_queue_empty_lifecycle_scenario() {
    detail::TaskResumeQueue queue;
    TaskRef task = make_task_ref();
    for (size_t round = 0; round < 32; ++round) {
        for (size_t i = 0; i < 4096; ++i) {
            TaskState* ready = queue.take_all();
            if (ready != nullptr) {
                detail::TaskResumeQueue::release_all(ready);
                std::cerr << "[T97] empty open queue returned a node\n";
                return false;
            }
        }
        if (!queue.push(task)) { return false; }
        queue.close();
        // A nonempty closed queue must retain its nodes and reject reopen.
        if (queue.reopen()) { return false; }
        TaskState* ready = queue.take_all();
        const bool single = ready == task.state() && ready->m_resume_queue_next == nullptr;
        detail::TaskResumeQueue::release_all(ready);
        if (!single || !queue.is_closed()) { return false; }
        for (size_t i = 0; i < 4096; ++i) {
            ready = queue.take_all();
            if (ready != nullptr || !queue.is_closed()) {
                detail::TaskResumeQueue::release_all(ready);
                std::cerr << "[T97] empty drain changed closed state\n";
                return false;
            }
        }
        if (queue.push(task) || task.state()->m_refs.load() != 1 ||
            task.state()->m_resume_queue_claimed.load() || !queue.reopen()) {
            return false;
        }
        if (!queue.push(task)) { return false; }
        detail::TaskResumeQueue::release_all(queue.take_all());
        if (!queue.empty() || task.state()->m_refs.load() != 1 ||
            task.state()->m_resume_queue_claimed.load()) { return false; }
    }
    std::cout << "T97 resume empty: open/closed/poll/refill/reopen/ownership PASS\n";
    return true;
}

bool run_resume_queue_concurrent_drain_scenario() {
    constexpr size_t kProducers = 4;
    constexpr size_t kPerProducer = 256;
    constexpr size_t kCount = kProducers * kPerProducer;
    for (size_t round = 0; round < 16; ++round) {
        detail::TaskResumeQueue queue;
        std::array<TaskRef, kCount> tasks;
        std::array<bool, kCount> accepted{};
        std::array<unsigned, kCount> seen{};
        for (TaskRef& task : tasks) { task = make_task_ref(); }
        std::atomic<bool> start{false};
        std::atomic<size_t> done{0};
        std::atomic<size_t> progressed{0};
        std::array<std::thread, kProducers> producers;
        for (size_t p = 0; p < kProducers; ++p) {
            producers[p] = std::thread([&, p]() {
                while (!start.load(std::memory_order_acquire)) { std::this_thread::yield(); }
                for (size_t n = 0; n < kPerProducer; ++n) {
                    const size_t i = p * kPerProducer + n;
                    accepted[i] = queue.push(tasks[i]);
                    progressed.fetch_add(1, std::memory_order_release);
                    if (n % 16 == 0) { std::this_thread::yield(); }
                }
                done.fetch_add(1, std::memory_order_release);
            });
        }
        std::thread closer([&]() {
            // Alternate open drain and close racing publication after some progress.
            const size_t threshold = round % 2 == 0 ? kCount : kCount / 4;
            while (progressed.load(std::memory_order_acquire) < threshold) {
                std::this_thread::yield();
            }
            queue.close();
        });
        bool valid = true;
        start.store(true, std::memory_order_release);
        do {
            TaskState* ready = queue.take_all();
            for (TaskState* node = ready; node != nullptr; node = node->m_resume_queue_next) {
                size_t i = 0;
                while (i < kCount && tasks[i].state() != node) { ++i; }
                if (i == kCount || ++seen[i] != 1) { valid = false; }
            }
            detail::TaskResumeQueue::release_all(ready);
            std::this_thread::yield();
        } while (done.load(std::memory_order_acquire) != kProducers || !queue.empty());
        for (std::thread& producer : producers) { producer.join(); }
        closer.join();
        for (size_t i = 0; i < kCount; ++i) {
            if (seen[i] != static_cast<unsigned>(accepted[i]) ||
                tasks[i].state()->m_refs.load() != 1 ||
                tasks[i].state()->m_resume_queue_claimed.load()) { valid = false; }
        }
        if (!valid || !queue.is_closed() || !queue.empty() || !queue.reopen() ||
            !queue.push(tasks.front())) {
            std::cerr << "[T97] concurrent drain lost/duplicated a task or ownership\n";
            return false;
        }
        detail::TaskResumeQueue::release_all(queue.take_all());
    }
    std::cout << "T97 resume MPSC: concurrent empty drain/publication/close/exactly-once PASS\n";
    return true;
}

}  // namespace

int main() {
    if (!run_scenario()) {
        return 1;
    }
    if (!run_resume_admission_scenario()) {
        return 1;
    }
    if (!run_resume_fifo_scenario()) {
        return 1;
    }
    if (!run_resume_batched_fifo_scenario(false) || !run_resume_batched_fifo_scenario(true)) {
        return 1;
    }
    if (!run_resume_admission_lifecycle_scenario()) {
        return 1;
    }
    if (!run_resume_fairness_scenario()) {
        return 1;
    }
    if (!run_resume_queue_close_scenario()) {
        return 1;
    }
    if (!run_resume_queue_duplicate_scenario()) {
        return 1;
    }
    if (!run_resume_queue_empty_lifecycle_scenario() ||
        !run_resume_queue_concurrent_drain_scenario()) {
        return 1;
    }
    if (!run_resume_queue_ownership_scenario()) {
        return 1;
    }
    if (!run_resume_queue_cross_queue_claim_scenario() ||
        !run_resume_queue_same_state_drain_scenario() ||
        !run_resume_duplicate_pending_scenario()) {
        return 1;
    }
    if (!run_mixed_drain_accounting_scenario() || !run_mixed_drain_concurrent_scenario()) {
        return 1;
    }
    std::cout << "T97-ioscheduler_inject_ring_fallback PASS\n";
    return 0;
}
