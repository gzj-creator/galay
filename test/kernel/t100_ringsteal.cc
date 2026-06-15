/**
 * @file t100_ringsteal.cc
 * @brief 用途：压测 Chase-Lev ring 在 owner pop 与 stealer steal 并发下的正确性。
 * 关键覆盖点：single-item CAS 竞争、0-3 元素波动、无丢失无重复。
 * 通过条件：10M 轮结束后消费总数等于生产总数，且没有重复消费。
 */

#include "galay-kernel/core/io_scheduler.hpp"

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace galay::kernel;

namespace {

constexpr uint64_t kRoundCount = 10'000'000;
constexpr int kStealerCount = 4;

TaskRef makeTaggedTask(uint64_t id) {
    auto* state = new TaskState(std::coroutine_handle<>{});
    state->m_runtime =
        reinterpret_cast<Runtime*>(static_cast<uintptr_t>(id + 1));
    return TaskRef(state, false);
}

uint64_t taggedTaskId(const TaskRef& task) {
    return static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(task.state()->m_runtime) - 1);
}

bool runStressScenario() {
    ChaseLevTaskRing ring;
    std::vector<std::atomic<uint8_t>> seen(kRoundCount);
    for (auto& slot : seen) {
        slot.store(0, std::memory_order_relaxed);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> owner_done{false};
    std::atomic<uint64_t> total_consumed{0};
    std::atomic<uint64_t> duplicates{0};

    auto consume = [&](TaskRef task) {
        const uint64_t id = taggedTaskId(task);
        if (id >= kRoundCount) {
            duplicates.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (seen[static_cast<size_t>(id)].fetch_add(1, std::memory_order_relaxed) != 0) {
            duplicates.fetch_add(1, std::memory_order_relaxed);
        }
        total_consumed.fetch_add(1, std::memory_order_relaxed);
    };

    std::thread owner([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t next_id = 0;
        while (next_id < kRoundCount ||
               total_consumed.load(std::memory_order_acquire) < kRoundCount) {
            const uint64_t target_depth = next_id & 0x3ULL;

            while (next_id < kRoundCount) {
                const uint64_t consumed = total_consumed.load(std::memory_order_relaxed);
                const uint64_t outstanding = next_id - consumed;
                if (outstanding > target_depth) {
                    break;
                }
                if (!ring.push_back(makeTaggedTask(next_id))) {
                    break;
                }
                ++next_id;
            }

            TaskRef task;
            if (ring.pop_back(task)) {
                consume(std::move(task));
                continue;
            }

            if (next_id >= kRoundCount &&
                total_consumed.load(std::memory_order_acquire) >= kRoundCount) {
                break;
            }

            std::this_thread::yield();
        }

        owner_done.store(true, std::memory_order_release);
    });

    std::vector<std::thread> stealers;
    stealers.reserve(kStealerCount);
    for (int i = 0; i < kStealerCount; ++i) {
        stealers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!owner_done.load(std::memory_order_acquire) ||
                   total_consumed.load(std::memory_order_acquire) < kRoundCount) {
                TaskRef task;
                if (ring.steal_front(task)) {
                    consume(std::move(task));
                    continue;
                }
                std::this_thread::yield();
            }
        });
    }

    start.store(true, std::memory_order_release);
    owner.join();
    for (auto& stealer : stealers) {
        stealer.join();
    }

    const uint64_t consumed = total_consumed.load(std::memory_order_acquire);
    if (consumed != kRoundCount) {
        std::cerr << "[T100] consumed count mismatch, expected=" << kRoundCount
                  << ", actual=" << consumed << "\n";
        return false;
    }

    const uint64_t duplicate_count = duplicates.load(std::memory_order_acquire);
    if (duplicate_count != 0) {
        std::cerr << "[T100] duplicate consumption detected, duplicates="
                  << duplicate_count << "\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runStressScenario()) {
        return 1;
    }

    std::cout << "T100-ChaseLevRingStealPopStress PASS\n";
    return 0;
}
