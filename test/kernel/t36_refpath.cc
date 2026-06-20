/**
 * @file t36_refpath.cc
 * @brief 用途：验证 `TaskRef` 走调度路径时会进入预期的 scheduler 恢复流程。
 * 关键覆盖点：TaskRef 调度入口、本地或远端派发、owner scheduler 恢复。
 * 通过条件：TaskRef 调度路径命中预期断言，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>
#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> pendingTask() {
    co_return;
}

class CaptureScheduler final : public Scheduler {
public:
    void start() override {}
    void stop() override {}

    bool schedule(TaskRef task) override {
        if (task.isValid()) {
            ++schedule_calls;
        }
        return true;
    }

    bool scheduleDeferred(TaskRef task) override {
        return schedule(std::move(task));
    }

    bool scheduleImmediately(TaskRef task) override {
        if (task.isValid()) {
            ++schedule_immediately_calls;
        }
        return true;
    }

    bool addTimer(Timer::ptr) override { return true; }

    SchedulerType type() override {
        return kIOScheduler;
    }

    int schedule_calls = 0;
    int schedule_immediately_calls = 0;
};

bool verifyWakerUsesTaskRefSchedule() {
    CaptureScheduler scheduler;
    Task<void> task = pendingTask();
    detail::setTaskScheduler(detail::TaskAccess::taskRef(task), &scheduler);

    Waker waker(detail::TaskAccess::taskRef(task));
    waker.wakeUp();

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

bool verifyTaskResumeHelperUsesTaskRefSchedule() {
    CaptureScheduler scheduler;
    Task<void> task = pendingTask();
    detail::setTaskScheduler(detail::TaskAccess::taskRef(task), &scheduler);

    if (!detail::requestTaskResume(detail::TaskAccess::taskRef(task))) {
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
    if (!verifyWakerUsesTaskRefSchedule()) {
        return 1;
    }
    if (!verifyTaskResumeHelperUsesTaskRefSchedule()) {
        return 1;
    }

    std::cout << "T36-TaskRefSchedulePath PASS\n";
    return 0;
}
