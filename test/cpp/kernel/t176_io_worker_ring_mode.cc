/**
 * @file t176_io_worker_ring_mode.cc
 * @brief 验证 owner 线程本地就绪栈的容量、顺序和所有权。
 */

#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/ready_entry.h>

#include <coroutine>
#include <array>
#include <iostream>

using namespace galay::kernel;

namespace {
struct FakeCoro {
    detail::ReadyEntryCoroHeader header;
    size_t releases = 0;
};

void release_fake(void* ptr) noexcept {
    ++static_cast<FakeCoro*>(ptr)->releases;
}

constexpr detail::ReadyEntryHooks kHooks{
    .owner_scheduler = nullptr,
    .resume = nullptr,
    .release = release_fake,
};

bool verify_empty_and_single_task() {
    FakeCoro coro{};
    coro.header.hooks = &kHooks;
    LocalReadyStack stack;
    detail::ReadyEntry out;
    for (size_t cycle = 0; cycle < 4096; ++cycle) {
        for (size_t poll = 0; poll < 8; ++poll) {
            if (stack.pop_back(out) || out.is_valid() || !stack.empty() ||
                stack.size() != 0 ||
                stack.remaining_capacity() != LocalReadyStack::kCapacity) {
                return false;
            }
        }
        detail::ReadyEntry entry(detail::ReadyEntryKind::CCoroutine, &coro);
        if (!stack.push_back(entry) || entry.is_valid() || stack.size() != 1 ||
            stack.remaining_capacity() != LocalReadyStack::kCapacity - 1 ||
            !stack.pop_back(out) || out.state() != &coro) {
            return false;
        }
        detail::release_ready_entry(out);
        if (coro.releases != cycle + 1 || !stack.empty()) {
            return false;
        }
    }
    detail::ReadyEntry invalid;
    return !stack.push_back(invalid);
}

bool verify_capacity_order_and_ownership() {
    constexpr size_t capacity = LocalReadyStack::kCapacity;
    std::array<TaskRef, capacity> tasks;
    std::array<FakeCoro, capacity> coros{};
    for (size_t i = 0; i < capacity; ++i) {
        tasks[i] = TaskRef(new TaskState(std::coroutine_handle<>{}), false);
        coros[i].header.hooks = &kHooks;
    }
    LocalReadyStack stack;
    detail::ReadyEntry out;
    for (size_t cycle = 0; cycle < 4; ++cycle) {
        for (size_t i = 0; i < capacity; ++i) {
            detail::ReadyEntry entry = (i % 2 == 0)
                ? detail::ReadyEntry(TaskRef(tasks[i]))
                : detail::ReadyEntry(detail::ReadyEntryKind::CCoroutine, &coros[i]);
            if (!stack.push_back(entry) || entry.is_valid()) { return false; }
        }
        detail::ReadyEntry extra{TaskRef(tasks[0])};
        if (stack.push_back(extra) || !extra.is_valid() ||
            stack.size() != capacity || stack.remaining_capacity() != 0) {
            return false;
        }
        detail::release_ready_entry(extra);
        for (size_t i = capacity; i > 0; --i) {
            const size_t index = i - 1;
            if (!stack.pop_back(out) ||
                out.state() != (index % 2 == 0
                    ? static_cast<void*>(tasks[index].state())
                    : static_cast<void*>(&coros[index]))) { return false; }
            detail::release_ready_entry(out);
        }
        if (!stack.empty() || stack.pop_back(out)) { return false; }
        for (size_t i = 0; i < capacity; ++i) {
            if (tasks[i].state()->m_refs.load() != 1 ||
                (i % 2 != 0 && coros[i].releases != cycle + 1)) { return false; }
        }
    }
    const size_t releases_before_clear = coros[0].releases;
    detail::ReadyEntry cpp{TaskRef(tasks[0])};
    detail::ReadyEntry c(detail::ReadyEntryKind::CCoroutine, &coros[0]);
    if (!stack.push_back(cpp) || !stack.push_back(c) || !stack.pop_back(out)) {
        return false;
    }
    detail::release_ready_entry(out);
    stack.clear();
    stack.clear();
    return tasks[0].state()->m_refs.load() == 1 &&
        coros[0].releases == releases_before_clear + 1 && stack.empty();
}
} // namespace

int main() {
    if (!verify_empty_and_single_task()) {
        std::cerr << "[T176] empty polling or single-task roundtrip failure\n";
        return 1;
    }
    if (!verify_capacity_order_and_ownership()) {
        std::cerr << "[T176] local stack capacity/order/ownership failure\n";
        return 1;
    }
    IOReadyQueue worker;

    TaskRef queued = TaskRef(new TaskState(std::coroutine_handle<>{}), false);
    TaskState* const queued_state = queued.state();
    if (!worker.local_stack.push_back(std::move(queued))) {
        std::cerr << "[T176] failed to populate local stack\n";
        return 1;
    }

    TaskRef popped;
    if (!worker.local_stack.pop_back(popped) ||
        !popped.is_valid() || popped.state() != queued_state) {
        std::cerr << "[T176] failed to pop the owner-local task\n";
        return 1;
    }

    std::cout << "T176-LocalReadyStack PASS\n";
    return 0;
}
