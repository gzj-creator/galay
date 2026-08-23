/**
 * @file t179_sequence_completion_once.cc
 * @brief 验证 sequence 完成清理可被 reactor 与 await_resume 重复调用。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/compute_scheduler.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <iostream>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using Result = std::expected<int, IOError>;

struct BindingProbe final : TimeoutTimerBinding {
    using TimeoutTimerBinding::forwardBoundTimeoutTimer;
};

void completionDoesNotRetainTimerPointer()
{
    using Sequence = SequenceAwaitable<Result, 1>;
    alignas(TimeoutTimer) std::byte storage[sizeof(TimeoutTimer)];
    auto* timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    Sequence sequence(nullptr);
    sequence.bindTimeoutTimer(timer);
    sequence.cancelBoundTimeoutTimer();

    std::destroy_at(timer);
    timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    sequence.cancelBoundTimeoutTimer();

    // 第二次完成通知不能再次解引用旧绑定。
    assert(!timer->cancelled());
    std::destroy_at(timer);
}

void closeAwaitableUsesTheSameBindingContract()
{
    alignas(TimeoutTimer) std::byte storage[sizeof(TimeoutTimer)];
    auto* timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    CloseAwaitable close(nullptr);
    close.bindTimeoutTimer(timer);
    close.cancelBoundTimeoutTimer();

    std::destroy_at(timer);
    timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    close.cancelBoundTimeoutTimer();
    assert(!timer->cancelled());
    std::destroy_at(timer);
}

Task<void> closeAwaitableImmediateProbe(TimeoutTimer* timer, bool* canceled)
{
    CloseAwaitable close(nullptr);
    close.bindTimeoutTimer(timer);
    (void)co_await close;
    *canceled = timer->cancelled();
}

void closeAwaitSuspendConsumesBinding()
{
    auto timer = TimeoutTimer::create(1h);
    bool canceled = false;
    ComputeScheduler scheduler;
    auto task = detail::TaskAccess::detachTask(
        closeAwaitableImmediateProbe(timer.get(), &canceled));
    assert(scheduler.scheduleImmediately(std::move(task)));
    assert(canceled);
}

void timeoutBindingCanMoveFromWrapperToInner()
{
    BindingProbe outer;
    BindingProbe inner;
    auto timer = TimeoutTimer::create(1h);

    outer.bindTimeoutTimer(timer.get());
    outer.forwardBoundTimeoutTimer(inner);
    outer.cancelBoundTimeoutTimer();
    assert(!timer->cancelled());
    inner.cancelBoundTimeoutTimer();
    assert(timer->cancelled());
}

void sequenceCompletionRemainsIdempotent()
{
    IOController controller(GHandle::invalid());
    SequenceAwaitable<Result, 1> sequence(&controller);
    assert(sequence.claimRequestedDomain());

    sequence.onCompleted();
    sequence.onCompleted();

    assert(!sequence.m_registered);
    assert(controller.m_sequence_owner[IOController::READ] == nullptr);
    assert(controller.m_sequence_owner[IOController::WRITE] == nullptr);
}

}  // namespace

int main()
{
    completionDoesNotRetainTimerPointer();
    closeAwaitableUsesTheSameBindingContract();
    closeAwaitSuspendConsumesBinding();
    timeoutBindingCanMoveFromWrapperToInner();
    sequenceCompletionRemainsIdempotent();
    std::cout << "T179-SequenceCompletionOnce PASS\n";
    return 0;
}
