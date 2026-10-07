/**
 * @file t158_timeout_transaction.cc
 * @brief 验证 TimeoutTimer 对破坏性 channel 操作提供可提交或回滚的完成权。
 */

#include <galay/cpp/galay-kernel/core/timeout.hpp>

#include <chrono>
#include <iostream>

namespace {

using galay::kernel::TimeoutTimer;
using galay::kernel::detail::DeferredWaker;
using namespace std::chrono_literals;

bool wake_before_arm_continues_synchronously()
{
    DeferredWaker waker;
    return waker.request_wake() && !waker.arm() &&
        !waker.request_wake();
}

bool wake_after_arm_is_issued_once()
{
    DeferredWaker waker;
    return waker.arm() && waker.request_wake() &&
        !waker.request_wake();
}

bool timer_wake_before_arm_continues_synchronously()
{
    TimeoutTimer timer(1h);
    timer.handle_timeout();
    return timer.timeouted() && !timer.arm_waker();
}

bool operation_commits_after_timeout_request()
{
    TimeoutTimer timer(1h);
    if (timer.try_begin_operation() !=
        TimeoutTimer::OperationStart::kStarted) {
        return false;
    }
    timer.handle_timeout();
    return !timer.timeouted() && timer.commit_operation() &&
        !timer.timeouted() && timer.cancelled();
}

bool timeout_completes_after_operation_abort()
{
    TimeoutTimer timer(1h);
    if (timer.try_begin_operation() !=
        TimeoutTimer::OperationStart::kStarted) {
        return false;
    }
    timer.handle_timeout();
    return timer.abort_operation() ==
            TimeoutTimer::OperationAbort::kTimeoutWon &&
        timer.timeouted();
}

bool operation_can_rearm_before_timeout()
{
    TimeoutTimer timer(1h);
    if (timer.try_begin_operation() !=
            TimeoutTimer::OperationStart::kStarted ||
        timer.try_begin_operation() !=
            TimeoutTimer::OperationStart::kBusy ||
        timer.abort_operation() !=
            TimeoutTimer::OperationAbort::kRearmed) {
        return false;
    }
    timer.handle_timeout();
    return timer.timeouted() &&
        timer.try_begin_operation() ==
            TimeoutTimer::OperationStart::kTimeoutWon;
}

bool committed_operation_stays_terminal()
{
    TimeoutTimer timer(1h);
    if (timer.try_begin_operation() !=
            TimeoutTimer::OperationStart::kStarted ||
        !timer.commit_operation()) {
        return false;
    }
    timer.handle_timeout();
    return !timer.timeouted() &&
        timer.try_begin_operation() ==
            TimeoutTimer::OperationStart::kOperationWon &&
        timer.abort_operation() ==
            TimeoutTimer::OperationAbort::kCompleted;
}

}  // namespace

int main()
{
    if (!wake_before_arm_continues_synchronously()) {
        std::cerr << "[T158] wake before arm did not stay synchronous\n";
        return 1;
    }
    if (!wake_after_arm_is_issued_once()) {
        std::cerr << "[T158] armed waker issued more than one wake\n";
        return 1;
    }
    if (!timer_wake_before_arm_continues_synchronously()) {
        std::cerr << "[T158] timer woke before await_suspend was armed\n";
        return 1;
    }
    if (!operation_commits_after_timeout_request()) {
        std::cerr << "[T158] operation commit lost to an in-flight timeout\n";
        return 1;
    }
    if (!timeout_completes_after_operation_abort()) {
        std::cerr << "[T158] deferred timeout did not complete after abort\n";
        return 1;
    }
    if (!operation_can_rearm_before_timeout()) {
        std::cerr << "[T158] aborted operation did not return to pending\n";
        return 1;
    }
    if (!committed_operation_stays_terminal()) {
        std::cerr << "[T158] committed operation was not terminal\n";
        return 1;
    }

    std::cout << "T158-TimeoutTransaction PASS\n";
    return 0;
}
