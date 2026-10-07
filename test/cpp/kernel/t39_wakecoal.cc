/**
 * @file t39_wakecoal.cc
 * @brief 用途：验证调度器会合并重复唤醒请求，避免无效的多次唤醒。
 * 关键覆盖点：重复 wake 请求合并、唤醒计数控制、可运行队列推进。
 * 通过条件：重复唤醒被成功压缩且任务仍能完整执行，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/sched_access.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <unistd.h>
#if defined(USE_KQUEUE)
#include <sys/event.h>
#endif

using namespace galay::kernel;

namespace {

Task<void> pending_task() {
    co_return;
}

template <typename SchedulerT>
bool start_wake_reactor(SchedulerT& scheduler) {
    auto started = SchedulerTestAccess::start_reactor(scheduler);
    if (!started) {
        std::cerr << "[T39] failed to start reactor: " << started.error().message() << "\n";
        return false;
    }
    return true;
}

bool schedule_injected_tasks(IOScheduler& scheduler, int count) {
    for (int i = 0; i < count; ++i) {
        Task<void> task = pending_task();
        detail::set_task_scheduler(detail::TaskAccess::task_ref(task), &scheduler);
        if (!scheduler.schedule(detail::TaskAccess::task_ref(task))) {
            std::cerr << "[T39] failed to inject task " << i << "\n";
            return false;
        }
    }
    return true;
}

#if defined(USE_KQUEUE)
bool read_kqueue_wake_events(int kqueue_fd, int& total) {
    total = 0;
    while (true) {
        struct kevent ev{};
        const timespec timeout{0, 0};
        const int n = kevent(kqueue_fd, nullptr, 0, &ev, 1, &timeout);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            std::cerr << "[T39] failed to read kqueue wake event: " << std::strerror(errno) << "\n";
            return false;
        }
        if ((ev.flags & EV_ERROR) != 0) {
            std::cerr << "[T39] unexpected EV_ERROR on wake event, data=" << ev.data << "\n";
            return false;
        }
        if (ev.filter != EVFILT_USER) {
            std::cerr << "[T39] expected EVFILT_USER wake event, filter=" << ev.filter << "\n";
            return false;
        }
        ++total;
    }
}

bool verify_wakeup_coalescing() {
    KqueueScheduler scheduler;

    if (!start_wake_reactor(scheduler)) {
        return false;
    }

    if (!schedule_injected_tasks(scheduler, 3)) {
        return false;
    }

    int total = 0;
    if (!read_kqueue_wake_events(SchedulerTestAccess::wake_read_fd(scheduler), total)) {
        return false;
    }

    if (total != 1) {
        std::cerr << "[T39] expected a single coalesced wakeup event, got " << total << "\n";
        return false;
    }

    return true;
}
#elif defined(USE_EPOLL)
bool verify_wakeup_coalescing() {
    EpollScheduler scheduler;

    if (!start_wake_reactor(scheduler)) {
        return false;
    }

    if (!schedule_injected_tasks(scheduler, 3)) {
        return false;
    }

    uint64_t wake_count = 0;
    const ssize_t n = read(SchedulerTestAccess::wake_read_fd(scheduler), &wake_count, sizeof(wake_count));
    if (n != static_cast<ssize_t>(sizeof(wake_count))) {
        std::cerr << "[T39] failed to read eventfd wake count\n";
        return false;
    }

    if (wake_count != 1) {
        std::cerr << "[T39] expected a single coalesced wakeup, got " << wake_count << "\n";
        return false;
    }

    return true;
}
#elif defined(USE_IOURING)
bool verify_wakeup_coalescing() {
    IOUringScheduler scheduler;

    if (!start_wake_reactor(scheduler)) {
        return false;
    }

    if (!schedule_injected_tasks(scheduler, 3)) {
        return false;
    }

    uint64_t wake_count = 0;
    const ssize_t n = read(SchedulerTestAccess::wake_read_fd(scheduler), &wake_count, sizeof(wake_count));
    if (n != static_cast<ssize_t>(sizeof(wake_count))) {
        std::cerr << "[T39] failed to read eventfd wake count\n";
        return false;
    }

    if (wake_count != 1) {
        std::cerr << "[T39] expected a single coalesced wakeup, got " << wake_count << "\n";
        return false;
    }

    return true;
}
#else
bool verify_wakeup_coalescing() {
    std::cout << "T39-SchedulerWakeupCoalescing SKIP\n";
    return true;
}
#endif

}  // namespace

int main() {
    if (!verify_wakeup_coalescing()) {
        return 1;
    }

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    std::cout << "T39-SchedulerWakeupCoalescing PASS\n";
#endif
    return 0;
}
