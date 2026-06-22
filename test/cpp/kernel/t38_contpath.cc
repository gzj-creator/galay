/**
 * @file t38_contpath.cc
 * @brief 用途：验证 `wait` continuation 会回到正确的 owner scheduler 上恢复。
 * 关键覆盖点：等待 continuation 绑定、跨调度器完成通知、恢复线程归属。
 * 通过条件：continuation 恢复到预期调度器且测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <cassert>
#include <deque>
#include <iostream>

using namespace galay::kernel;

namespace {

struct ChildSuspendState {
    Waker waker;
    bool armed = false;
    bool child_done = false;
};

struct ParentState {
    bool parent_done = false;
    int parent_resumes = 0;
};

class ManualScheduler final : public Scheduler {
public:
    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}

    bool schedule(TaskRef task) override {
        if (!bindTask(task)) {
            return false;
        }
        ++schedule_calls;
        m_ready.push_back(std::move(task));
        return true;
    }

    bool scheduleDeferred(TaskRef task) override {
        return schedule(std::move(task));
    }

    bool scheduleImmediately(TaskRef task) override {
        if (!bindTask(task)) {
            return false;
        }
        resume(task);
        return true;
    }

    bool addTimer(Timer::ptr) override { return true; }

    SchedulerType type() override {
        return kComputeScheduler;
    }

    bool runOne() {
        if (m_ready.empty()) {
            return false;
        }
        TaskRef task = std::move(m_ready.front());
        m_ready.pop_front();
        resume(task);
        return true;
    }

    int schedule_calls = 0;

private:
    std::deque<TaskRef> m_ready;
};

struct ChildSuspendAwaitable {
    ChildSuspendState* state;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        state->waker = Waker(handle);
        state->armed = true;
        return true;
    }

    void await_resume() const noexcept {}
};

Task<void> childTask(ChildSuspendState* state) {
    co_await ChildSuspendAwaitable{state};
    state->child_done = true;
    co_return;
}

Task<void> parentTask(ChildSuspendState* child_state, ParentState* parent_state) {
    auto child_result = co_await childTask(child_state);
    assert(child_result.has_value());
    ++parent_state->parent_resumes;
    parent_state->parent_done = true;
    co_return;
}

bool verifyWaitContinuationReturnsThroughScheduler() {
    ManualScheduler scheduler;
    ChildSuspendState child_state;
    ParentState parent_state;

    if (!scheduler.scheduleImmediately(detail::TaskAccess::detachTask(
            parentTask(&child_state, &parent_state)))) {
        std::cerr << "[T38] failed to start parent task\n";
        return false;
    }

    if (!child_state.armed) {
        std::cerr << "[T38] child did not suspend as expected\n";
        return false;
    }

    child_state.waker.wakeUp();
    if (scheduler.schedule_calls != 1) {
        std::cerr << "[T38] expected child wake to schedule once, got "
                  << scheduler.schedule_calls << "\n";
        return false;
    }

    if (!scheduler.runOne()) {
        std::cerr << "[T38] expected child task in ready queue\n";
        return false;
    }

    if (!child_state.child_done) {
        std::cerr << "[T38] child did not complete after resume\n";
        return false;
    }

    if (scheduler.schedule_calls != 2) {
        std::cerr << "[T38] expected waiter continuation to be re-scheduled, got "
                  << scheduler.schedule_calls << " schedule calls\n";
        return false;
    }

    if (parent_state.parent_done) {
        std::cerr << "[T38] waiter resumed inline instead of being queued\n";
        return false;
    }

    if (!scheduler.runOne()) {
        std::cerr << "[T38] expected waiter continuation in ready queue\n";
        return false;
    }

    if (!parent_state.parent_done || parent_state.parent_resumes != 1) {
        std::cerr << "[T38] waiter did not resume exactly once\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!verifyWaitContinuationReturnsThroughScheduler()) {
        return 1;
    }

    std::cout << "T38-WaitContinuationSchedulerPath PASS\n";
    return 0;
}
