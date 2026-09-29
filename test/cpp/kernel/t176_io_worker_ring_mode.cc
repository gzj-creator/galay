/**
 * @file t176_io_worker_ring_mode.cc
 * @brief 验证 IO worker 的 stealing 开关会传递到本地 Chase-Lev ring。
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
} // namespace

int main() {
    if (!verifyOwnerOnlyLifecycle()) {
        std::cerr << "[T176] owner-only capacity/order/ownership failure\n";
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
