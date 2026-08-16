/**
 * @file t176_io_worker_ring_mode.cc
 * @brief 验证 IO worker 的 stealing 开关会传递到本地 Chase-Lev ring。
 */

#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <coroutine>
#include <iostream>

using namespace galay::kernel;

int main() {
    IOSchedulerWorkerState worker;
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
