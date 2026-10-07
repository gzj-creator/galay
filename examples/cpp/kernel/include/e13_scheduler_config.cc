/**
 * @file e13_scheduler_config.cc
 * @brief 自定义当前构建所选 IO 后端的容量，并提交一个会让出执行权的协程。
 *
 * Config 只配置容量，不替换 reactor、poll/flush 策略或事件循环实现。
 * Runtime 当前不支持注入自定义调度器，因此这里独立管理调度器生命周期。
 * 通过条件：任务经过 yield 后恢复完成，调度器正常停止，程序返回 0。
 */

#include <galay/cpp/galay-kernel/common/scheduler_config.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/scheduler_dispatch.hpp>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

// 参数依次为：epoll/kqueue 的事件容量、协程批量大小、io_uring 队列深度。
// 这些值只是演示配置，不代表针对特定业务调优后的建议值。
using CustomConfig = IOSchedulerConfig<2048, 128, 4096>;

#if defined(USE_IOURING)
using CustomScheduler = IOUringSchedulerT<CustomConfig>;
#elif defined(USE_KQUEUE)
using CustomScheduler = KqueueSchedulerT<CustomConfig>;
#elif defined(USE_EPOLL)
using CustomScheduler = EpollSchedulerT<CustomConfig>;
#else
#error "An IO scheduler backend must be selected"
#endif

Task<void> demo_task(std::atomic<bool>* done) {
    co_yield true;
    done->store(true, std::memory_order_release);
}

}  // namespace

int main() {
    // 任务借用的状态必须活到 scheduler.stop() 完成之后。
    std::atomic<bool> done{false};
    CustomScheduler scheduler;
    const auto started = scheduler.start();
    if (!started) {
        std::cerr << "scheduler failed to start: " << started.error().message() << '\n';
        return 1;
    }

    // 同一后端的自定义配置也可以安全地通过 Scheduler* 提交任务。
    Scheduler* borrowed = &scheduler;
    if (!schedule_task(borrowed, demo_task(&done))) {
        scheduler.stop();
        std::cerr << "failed to submit task\n";
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool completed = false;
    while (!(completed = done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline) {
        // 仅主线程等待；协程本身通过 co_yield 让出执行权，不阻塞调度线程。
        std::this_thread::sleep_for(1ms);
    }
    scheduler.stop();
    if (!completed) {
        std::cerr << "task did not complete before deadline\n";
        return 1;
    }

#if defined(USE_IOURING)
    std::cout << "io_uring: queue_depth=" << CustomScheduler::queue_depth();
#elif defined(USE_KQUEUE)
    std::cout << "kqueue: max_events=" << CustomScheduler::max_events();
#else
    std::cout << "epoll: max_events=" << CustomScheduler::max_events();
#endif
    std::cout << ", batch_size=" << CustomScheduler::batch_size()
              << "; task resumed successfully\n";
    return 0;
}
