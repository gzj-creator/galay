/**
 * @file t103_ringshutdown.cc
 * @brief 验证 Chase-Lev ring 析构时会释放仍排队的任务引用。
 */

#include "kernel/kernel/io_scheduler.hpp"
#include "kernel/kernel/task.h"

#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> emptyTask() {
    co_return;
}

bool runScenario() {
    TaskRef queued = detail::TaskAccess::detachTask(emptyTask());
    TaskRef keeper = queued;
    auto* state = keeper.state();
    if (state == nullptr) {
        std::cerr << "[T103] keeper lost task state before enqueue\n";
        return false;
    }
    const uint32_t refs_before_destroy = state->m_refs.load(std::memory_order_acquire);

    {
        ChaseLevTaskRing ring;
        if (!ring.push_back(std::move(queued))) {
            std::cerr << "[T103] failed to enqueue task into ring\n";
            return false;
        }
    }

    const uint32_t refs_after_destroy = state->m_refs.load(std::memory_order_acquire);
    if (refs_after_destroy + 1 != refs_before_destroy) {
        std::cerr << "[T103] ring destruction should release exactly one queued task ref, before="
                  << refs_before_destroy << ", after=" << refs_after_destroy << "\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runScenario()) {
        return 1;
    }

    std::cout << "T103-ChaseLevRingShutdownRelease PASS\n";
    return 0;
}
