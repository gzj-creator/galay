/**
 * @file t176_io_worker_ring_mode.cc
 * @brief 验证 IO ring 的 owner-only 生命周期、槽位复用与停机后的模式切换。
 */

#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <coroutine>
#include <array>
#include <iostream>

using namespace galay::kernel;

namespace {
struct FakeCoro {
    detail::ReadyEntryCoroHeader header;
    size_t releases = 0;
};

void releaseFake(void* ptr) noexcept {
    ++static_cast<FakeCoro*>(ptr)->releases;
}

constexpr detail::ReadyEntryHooks kHooks{
    .owner_scheduler = nullptr,
    .resume_owner_only = nullptr,
    .resume = nullptr,
    .release = releaseFake,
};

bool verifyEmptyAndSingleTask() {
    for (const size_t advance : {size_t(0), ChaseLevTaskRing::kCapacity - 5}) {
        FakeCoro coro{};
        coro.header.hooks = &kHooks;
        ChaseLevTaskRing ring;
        detail::ReadyEntry out;
        for (size_t i = 0; i < advance; ++i) {
            detail::ReadyEntry entry(detail::ReadyEntryKind::CCoroutine, &coro);
            if (!ring.push_back(entry) || !ring.steal_front(out)) { return false; }
            detail::releaseReadyEntry(out);
        }
        ring.setStealingEnabled(false);
        for (size_t cycle = 0; cycle < 4096; ++cycle) {
            // Cover both initial and advanced empty cursors, then a single task.
            for (size_t poll = 0; poll < 8; ++poll) {
                if (ring.pop_back(out) || out.isValid() || !ring.empty() ||
                    ring.remainingCapacity() != ChaseLevTaskRing::kCapacity) {
                    return false;
                }
            }
            detail::ReadyEntry entry(detail::ReadyEntryKind::CCoroutine, &coro);
            if (!ring.push_back(entry) || entry.isValid() || ring.size() != 1 ||
                !ring.pop_back(out) || out.state() != &coro) { return false; }
            detail::releaseReadyEntry(out);
            if (coro.releases != advance + cycle + 1 || !ring.empty()) { return false; }
        }
    }
    return true;
}

bool verifyOwnerOnlyLifecycle() {
    constexpr size_t capacity = ChaseLevTaskRing::kCapacity;
    std::array<TaskRef, capacity> tasks;
    std::array<FakeCoro, capacity> coros{};
    for (size_t i = 0; i < capacity; ++i) {
        tasks[i] = TaskRef(new TaskState(std::coroutine_handle<>{}), false);
        coros[i].header.hooks = &kHooks;
    }
    ChaseLevTaskRing ring;
    detail::ReadyEntry out;
    // Advance both cursors before entering owner-only mode to exercise wraparound.
    for (size_t i = 0; i < capacity - 5; ++i) {
        detail::ReadyEntry seed{TaskRef(tasks[0])};
        if (!ring.push_back(seed) || !ring.steal_front(out)) { return false; }
        detail::releaseReadyEntry(out);
    }
    ring.setStealingEnabled(false);
    if (ring.pop_back(out) || out.isValid()) { return false; }
    for (size_t cycle = 0; cycle < 16; ++cycle) {
        // Empty polling must preserve the advanced cursors and full refill capacity.
        for (size_t poll = 0; poll < capacity; ++poll) {
            if (ring.pop_back(out) || out.isValid() || !ring.empty() ||
                ring.size() != 0 || ring.remainingCapacity() != capacity) { return false; }
        }
        for (size_t i = 0; i < capacity; ++i) {
            detail::ReadyEntry entry = (i % 2 == 0)
                ? detail::ReadyEntry(TaskRef(tasks[i]))
                : detail::ReadyEntry(detail::ReadyEntryKind::CCoroutine, &coros[i]);
            if (!ring.push_back(entry) || entry.isValid()) { return false; }
        }
        detail::ReadyEntry extra{TaskRef(tasks[0])};
        if (ring.push_back(extra) || !extra.isValid() ||
            ring.steal_front(out) || out.isValid()) { return false; }
        detail::releaseReadyEntry(extra);
        for (size_t i = capacity; i > 0; --i) {
            const size_t index = i - 1;
            if (!ring.pop_back(out) ||
                out.state() != (index % 2 == 0
                    ? static_cast<void*>(tasks[index].state())
                    : static_cast<void*>(&coros[index]))) { return false; }
            detail::releaseReadyEntry(out);
        }
        if (!ring.empty() || ring.pop_back(out)) { return false; }
        for (size_t i = 0; i < capacity; ++i) {
            if (tasks[i].state()->m_refs.load() != 1 ||
                (i % 2 != 0 && coros[i].releases != cycle + 1)) { return false; }
        }
    }
    // Clear a partially drained mixed ring and ensure consumed slots stay empty.
    detail::ReadyEntry cpp{TaskRef(tasks[0])};
    detail::ReadyEntry c(detail::ReadyEntryKind::CCoroutine, &coros[0]);
    if (!ring.push_back(cpp) || !ring.push_back(c) || !ring.pop_back(out)) { return false; }
    detail::releaseReadyEntry(out);
    ring.clear();
    ring.clear();
    return tasks[0].state()->m_refs.load() == 1 && coros[0].releases == 1 && ring.empty();
}

bool verifyReenableStealing() {
    TaskRef task(new TaskState(std::coroutine_handle<>{}), false);
    ChaseLevTaskRing ring;
    detail::ReadyEntry out;
    for (size_t cycle = 0; cycle < 4; ++cycle) {
        ring.setStealingEnabled(false);
        detail::ReadyEntry invalid;
        if (ring.push_back(invalid)) { return false; }
        for (size_t i = 0; i < ChaseLevTaskRing::kCapacity; ++i) {
            detail::ReadyEntry entry{TaskRef(task)};
            if (!ring.push_back(entry)) { return false; }
        }
        for (size_t i = 0; i < ChaseLevTaskRing::kCapacity; ++i) {
            if (!ring.pop_back(out) || out.state() != task.state()) { return false; }
            detail::releaseReadyEntry(out);
        }
        // With no workers active, reuse every owner-drained slot via the CAS path.
        // A load-only pop would leave stale entries and reject these pushes.
        ring.setStealingEnabled(true);
        for (size_t i = 0; i < ChaseLevTaskRing::kCapacity; ++i) {
            detail::ReadyEntry entry{TaskRef(task)};
            if (!ring.push_back(entry)) { return false; }
        }
        for (size_t i = 0; i < ChaseLevTaskRing::kCapacity; ++i) {
            const bool popped = i % 2 == 0 ? ring.pop_back(out) : ring.steal_front(out);
            if (!popped || out.state() != task.state()) { return false; }
            detail::releaseReadyEntry(out);
        }
        if (!ring.empty() || ring.pop_back(out) || ring.steal_front(out) ||
            task.state()->m_refs.load() != 1) { return false; }
    }
    return true;
}
} // namespace

int main() {
    if (!verifyEmptyAndSingleTask()) {
        std::cerr << "[T176] empty polling or single-task roundtrip failure\n";
        return 1;
    }
    if (!verifyOwnerOnlyLifecycle()) {
        std::cerr << "[T176] owner-only capacity/order/ownership failure\n";
        return 1;
    }
    if (!verifyReenableStealing()) {
        std::cerr << "[T176] re-enabled ring retained consumed slots or references\n";
        return 1;
    }
    IOReadyQueue worker;
    worker.setStealingEnabled(false);

    TaskRef queued(new TaskState(std::coroutine_handle<>{}), false);
    if (!worker.local_ring.push_back(std::move(queued))) {
        std::cerr << "[T176] failed to populate local ring\n";
        return 1;
    }

    TaskRef stolen;
    if (worker.stealFront(stolen)) {
        std::cerr << "[T176] disabled IO worker still allowed local-ring stealing\n";
        return 1;
    }

    TaskRef popped;
    if (!worker.local_ring.pop_back(popped) || !popped.isValid()) {
        std::cerr << "[T176] rejected steal removed the owner task\n";
        return 1;
    }

    std::cout << "T176-IOWorkerRingMode PASS\n";
    return 0;
}
