/**
 * @file t42_qwake.cc
 * @brief 用途：验证队列从空到非空的边沿变化会触发正确的唤醒语义。
 * 关键覆盖点：空队列边沿检测、首次入队唤醒、重复入队避免冗余 wake。
 * 通过条件：边沿唤醒语义成立且测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include "test/sched_access.h"

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

Task<void> pendingTask() {
    co_return;
}

template <typename SchedulerT>
bool injectBurstFromEmptyQueue(SchedulerT& scheduler, int count) {
    SchedulerTestAccess::sleeping(scheduler).store(false, std::memory_order_release);
    SchedulerTestAccess::wakeupPending(scheduler).store(false, std::memory_order_release);

    for (int i = 0; i < count; ++i) {
        Task<void> task = pendingTask();
        detail::setTaskScheduler(detail::TaskAccess::taskRef(task), &scheduler);
        if (!scheduler.schedule(detail::TaskAccess::taskRef(task))) {
            std::cerr << "[T42] failed to inject task " << i << "\n";
            return false;
        }
    }
    return true;
}

#if defined(USE_KQUEUE)
bool readKqueueWakeEvents(int kqueue_fd, int& total) {
    total = 0;
    while (true) {
        struct kevent ev{};
        const timespec timeout{0, 0};
        const int n = kevent(kqueue_fd, nullptr, 0, &ev, 1, &timeout);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            std::cerr << "[T42] failed to read kqueue wake event: " << std::strerror(errno) << "\n";
            return false;
        }
        if ((ev.flags & EV_ERROR) != 0) {
            std::cerr << "[T42] unexpected EV_ERROR on wake event, data=" << ev.data << "\n";
            return false;
        }
        if (ev.filter != EVFILT_USER) {
            std::cerr << "[T42] expected EVFILT_USER wake event, filter=" << ev.filter << "\n";
            return false;
        }
        ++total;
    }
}

bool verifyQueueEdgeWakeup() {
    KqueueScheduler scheduler;

    if (!injectBurstFromEmptyQueue(scheduler, 3)) {
        return false;
    }

    int total = 0;
    if (!readKqueueWakeEvents(SchedulerTestAccess::wakeReadFd(scheduler), total)) {
        return false;
    }

    if (total != 1) {
        std::cerr << "[T42] expected a single edge-triggered wakeup event, got " << total << "\n";
        return false;
    }

    return true;
}
#elif defined(USE_EPOLL)
bool verifyQueueEdgeWakeup() {
    EpollScheduler scheduler;

    if (!injectBurstFromEmptyQueue(scheduler, 3)) {
        return false;
    }

    uint64_t wake_count = 0;
    const ssize_t n = read(SchedulerTestAccess::wakeReadFd(scheduler), &wake_count, sizeof(wake_count));
    if (n != static_cast<ssize_t>(sizeof(wake_count))) {
        std::cerr << "[T42] failed to read eventfd wake count\n";
        return false;
    }

    if (wake_count != 1) {
        std::cerr << "[T42] expected a single edge-triggered wakeup, got " << wake_count << "\n";
        return false;
    }

    return true;
}
#elif defined(USE_IOURING)
bool verifyQueueEdgeWakeup() {
    IOUringScheduler scheduler;

    if (!injectBurstFromEmptyQueue(scheduler, 3)) {
        return false;
    }

    uint64_t wake_count = 0;
    const ssize_t n = read(SchedulerTestAccess::wakeReadFd(scheduler), &wake_count, sizeof(wake_count));
    if (n != static_cast<ssize_t>(sizeof(wake_count))) {
        std::cerr << "[T42] failed to read eventfd wake count\n";
        return false;
    }

    if (wake_count != 1) {
        std::cerr << "[T42] expected a single edge-triggered wakeup, got " << wake_count << "\n";
        return false;
    }

    return true;
}
#else
bool verifyQueueEdgeWakeup() {
    std::cout << "T42-SchedulerQueueEdgeWakeup SKIP\n";
    return true;
}
#endif

}  // namespace

int main() {
    if (!verifyQueueEdgeWakeup()) {
        return 1;
    }

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    std::cout << "T42-SchedulerQueueEdgeWakeup PASS\n";
#endif
    return 0;
}
