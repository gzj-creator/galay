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

bool wakeBeforeArmContinuesSynchronously()
{
    DeferredWaker waker;
    return waker.requestWake() && !waker.arm() &&
        !waker.requestWake();
}

bool wakeAfterArmIsIssuedOnce()
{
    DeferredWaker waker;
    return waker.arm() && waker.requestWake() &&
        !waker.requestWake();
}

bool timerWakeBeforeArmContinuesSynchronously()
{
    TimeoutTimer timer(1h);
    timer.handleTimeout();
    return timer.timeouted() && !timer.armWaker();
}

bool operationCommitsAfterTimeoutRequest()
{
    TimeoutTimer timer(1h);
    if (timer.tryBeginOperation() !=
        TimeoutTimer::OperationStart::kStarted) {
        return false;
    }
    timer.handleTimeout();
    return !timer.timeouted() && timer.commitOperation() &&
        !timer.timeouted() && timer.cancelled();
}

bool timeoutCompletesAfterOperationAbort()
{
    TimeoutTimer timer(1h);
    if (timer.tryBeginOperation() !=
        TimeoutTimer::OperationStart::kStarted) {
        return false;
    }
    timer.handleTimeout();
    return timer.abortOperation() ==
            TimeoutTimer::OperationAbort::kTimeoutWon &&
        timer.timeouted();
}

bool operationCanRearmBeforeTimeout()
{
    TimeoutTimer timer(1h);
    if (timer.tryBeginOperation() !=
            TimeoutTimer::OperationStart::kStarted ||
        timer.tryBeginOperation() !=
            TimeoutTimer::OperationStart::kBusy ||
        timer.abortOperation() !=
            TimeoutTimer::OperationAbort::kRearmed) {
        return false;
    }
    timer.handleTimeout();
    return timer.timeouted() &&
        timer.tryBeginOperation() ==
            TimeoutTimer::OperationStart::kTimeoutWon;
}

bool committedOperationStaysTerminal()
{
    TimeoutTimer timer(1h);
    if (timer.tryBeginOperation() !=
            TimeoutTimer::OperationStart::kStarted ||
        !timer.commitOperation()) {
        return false;
    }
    timer.handleTimeout();
    return !timer.timeouted() &&
        timer.tryBeginOperation() ==
            TimeoutTimer::OperationStart::kOperationWon &&
        timer.abortOperation() ==
            TimeoutTimer::OperationAbort::kCompleted;
}

}  // namespace

int main()
{
    if (!wakeBeforeArmContinuesSynchronously()) {
        std::cerr << "[T158] wake before arm did not stay synchronous\n";
        return 1;
    }
    if (!wakeAfterArmIsIssuedOnce()) {
        std::cerr << "[T158] armed waker issued more than one wake\n";
        return 1;
    }
    if (!timerWakeBeforeArmContinuesSynchronously()) {
        std::cerr << "[T158] timer woke before await_suspend was armed\n";
        return 1;
    }
    if (!operationCommitsAfterTimeoutRequest()) {
        std::cerr << "[T158] operation commit lost to an in-flight timeout\n";
        return 1;
    }
    if (!timeoutCompletesAfterOperationAbort()) {
        std::cerr << "[T158] deferred timeout did not complete after abort\n";
        return 1;
    }
    if (!operationCanRearmBeforeTimeout()) {
        std::cerr << "[T158] aborted operation did not return to pending\n";
        return 1;
    }
    if (!committedOperationStaysTerminal()) {
        std::cerr << "[T158] committed operation was not terminal\n";
        return 1;
    }

    std::cout << "T158-TimeoutTransaction PASS\n";
    return 0;
}
