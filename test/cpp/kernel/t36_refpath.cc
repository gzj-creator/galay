/**
 * @file t36_refpath.cc
 * @brief 用途：验证 `TaskRef` 走调度路径时会进入预期的 scheduler 恢复流程。
 * 关键覆盖点：TaskRef 调度入口、本地或远端派发、owner scheduler 恢复。
 * 通过条件：TaskRef 调度路径命中预期断言，测试返回 0。
 */

#include "test/cpp/common/scheduler_test_adapter.h"
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>
#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> pending_task() {
    co_return;
}

class CaptureScheduler final : public detail::SchedulerTestAdapter<CaptureScheduler> {
public:
    std::expected<void, IOError> start() { return {}; }
    void stop() {}

    bool schedule(TaskRef task) noexcept {
        if (task.is_valid()) {
            ++schedule_calls;
        }
        return true;
    }

    bool schedule_resume(TaskRef task) noexcept {
        return schedule(std::move(task));
    }

    bool schedule_deferred(TaskRef task) noexcept {
        return schedule(std::move(task));
    }

    bool schedule_immediately(TaskRef task) noexcept {
        if (task.is_valid()) {
            ++schedule_immediately_calls;
        }
        return true;
    }

    bool add_timer(Timer::ptr) { return true; }

    SchedulerType type() {
        return kIOScheduler;
    }

    int schedule_calls = 0;
    int schedule_immediately_calls = 0;
};

bool verify_waker_uses_task_ref_schedule() {
    CaptureScheduler scheduler;
    Task<void> task = pending_task();
    detail::set_task_scheduler(detail::TaskAccess::task_ref(task), &scheduler);

    Waker waker(detail::TaskAccess::task_ref(task));
    waker.wake_up();

    if (scheduler.schedule_calls != 1) {
        std::cerr << "[T36] expected Waker::wakeUp to call schedule once, got "
                  << scheduler.schedule_calls << "\n";
        return false;
    }
    if (scheduler.schedule_immediately_calls != 0) {
        std::cerr << "[T36] expected Waker::wakeUp not to call scheduleImmediately, got "
                  << scheduler.schedule_immediately_calls << "\n";
        return false;
    }
    return true;
}

bool verify_task_resume_helper_uses_task_ref_schedule() {
    CaptureScheduler scheduler;
    Task<void> task = pending_task();
    detail::set_task_scheduler(detail::TaskAccess::task_ref(task), &scheduler);

    if (!detail::request_task_resume(detail::TaskAccess::task_ref(task))) {
        std::cerr << "[T36] expected requestTaskResume to schedule pending task\n";
        return false;
    }

    if (scheduler.schedule_calls != 1) {
        std::cerr << "[T36] expected requestTaskResume to call schedule once, got "
                  << scheduler.schedule_calls << "\n";
        return false;
    }
    if (scheduler.schedule_immediately_calls != 0) {
        std::cerr << "[T36] expected requestTaskResume not to call scheduleImmediately, got "
                  << scheduler.schedule_immediately_calls << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!verify_waker_uses_task_ref_schedule()) {
        return 1;
    }
    if (!verify_task_resume_helper_uses_task_ref_schedule()) {
        return 1;
    }

    std::cout << "T36-TaskRefSchedulePath PASS\n";
    return 0;
}
