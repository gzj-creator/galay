/**
 * @file t179_sequence_completion_once.cc
 * @brief 验证 sequence 完成清理可被 reactor 与 await_resume 重复调用。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>

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
    using TimeoutTimerBinding::forward_bound_timeout_timer;
};

void completion_does_not_retain_timer_pointer()
{
    using Sequence = SequenceAwaitable<Result, 1>;
    alignas(TimeoutTimer) std::byte storage[sizeof(TimeoutTimer)];
    auto* timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    Sequence sequence(nullptr);
    sequence.bind_timeout_timer(timer);
    sequence.cancel_bound_timeout_timer();

    std::destroy_at(timer);
    timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    sequence.cancel_bound_timeout_timer();

    // 第二次完成通知不能再次解引用旧绑定。
    assert(!timer->cancelled());
    std::destroy_at(timer);
}

void close_awaitable_uses_the_same_binding_contract()
{
    alignas(TimeoutTimer) std::byte storage[sizeof(TimeoutTimer)];
    auto* timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    CloseAwaitable close(nullptr);
    close.bind_timeout_timer(timer);
    close.cancel_bound_timeout_timer();

    std::destroy_at(timer);
    timer = std::construct_at(reinterpret_cast<TimeoutTimer*>(storage), 1h);
    close.cancel_bound_timeout_timer();
    assert(!timer->cancelled());
    std::destroy_at(timer);
}

Task<void> close_awaitable_immediate_probe(TimeoutTimer* timer, bool* canceled)
{
    CloseAwaitable close(nullptr);
    close.bind_timeout_timer(timer);
    (void)co_await close;
    *canceled = timer->cancelled();
}

void close_await_suspend_consumes_binding()
{
    auto timer = TimeoutTimer::create(1h);
    bool canceled = false;
    ParallelScheduler scheduler;
    auto task = detail::TaskAccess::detach_task(
        close_awaitable_immediate_probe(timer.get(), &canceled));
    assert(scheduler.schedule_immediately(std::move(task)));
    assert(canceled);
}

void timeout_binding_can_move_from_wrapper_to_inner()
{
    BindingProbe outer;
    BindingProbe inner;
    auto timer = TimeoutTimer::create(1h);

    outer.bind_timeout_timer(timer.get());
    outer.forward_bound_timeout_timer(inner);
    outer.cancel_bound_timeout_timer();
    assert(!timer->cancelled());
    inner.cancel_bound_timeout_timer();
    assert(timer->cancelled());
}

void sequence_completion_remains_idempotent()
{
    IOController controller(GHandle::invalid());
    SequenceAwaitable<Result, 1> sequence(&controller);
    assert(sequence.claim_requested_domain());

    sequence.on_completed();
    sequence.on_completed();

    assert(!sequence.m_registered);
    assert(controller.m_sequence_owner[IOController::READ] == nullptr);
    assert(controller.m_sequence_owner[IOController::WRITE] == nullptr);
}

}  // namespace

int main()
{
    completion_does_not_retain_timer_pointer();
    close_awaitable_uses_the_same_binding_contract();
    close_await_suspend_consumes_binding();
    timeout_binding_can_move_from_wrapper_to_inner();
    sequence_completion_remains_idempotent();
    std::cout << "T179-SequenceCompletionOnce PASS\n";
    return 0;
}
