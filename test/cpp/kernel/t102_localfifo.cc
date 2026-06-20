/**
 * @file t102_localfifo.cc
 * @brief 验证 owner 线程上的 deferred 本地任务仍保持 FIFO 语义。
 */

#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>

#include <coroutine>
#include <cstdint>
#include <iostream>

using namespace galay::kernel;

namespace {

TaskRef makeTaggedTask(uint64_t id) {
    auto* state = new TaskState(std::coroutine_handle<>{});
    state->m_runtime = reinterpret_cast<Runtime*>(static_cast<uintptr_t>(id + 1));
    return TaskRef(state, false);
}

uint64_t taggedTaskId(const TaskRef& task) {
    return static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(task.state()->m_runtime) - 1);
}

bool runScenario() {
    IOSchedulerWorkerState worker;

    worker.scheduleLocalDeferred(makeTaggedTask(0));
    worker.scheduleLocalDeferred(makeTaggedTask(1));
    worker.scheduleLocalDeferred(makeTaggedTask(2));

    const size_t drained = worker.drainInjected();
    if (drained != 3) {
        std::cerr << "[T102] expected deferred staging to drain 3 tasks, actual="
                  << drained << "\n";
        return false;
    }

    for (uint64_t expected = 0; expected < 3; ++expected) {
        TaskRef next;
        if (!worker.popNext(next)) {
            std::cerr << "[T102] expected deferred task " << expected << " to be available\n";
            return false;
        }
        const uint64_t actual = taggedTaskId(next);
        if (actual != expected) {
            std::cerr << "[T102] deferred FIFO violated, expected=" << expected
                      << ", actual=" << actual << "\n";
            return false;
        }
    }

    if (worker.hasLocalWork() || worker.hasPendingInjected()) {
        std::cerr << "[T102] worker should be empty after draining deferred FIFO\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runScenario()) {
        return 1;
    }

    std::cout << "T102-IOSchedulerLocalDeferredFIFO PASS\n";
    return 0;
}
