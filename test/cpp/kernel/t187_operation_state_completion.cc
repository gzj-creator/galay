/** @file t187_operation_state_completion.cc
 *  @brief Owner-thread 状态机的确定性顺序矩阵；不把并发直接写 state 当成合法 API。 */
#include <galay/cpp/galay-kernel/core/operation_state.hpp>

#if defined(GALAY_KERNEL_TASK_H) || defined(GALAY_KERNEL_IOCONTROLLER_HPP)
#error "OperationState must remain independent of coroutine and resource storage"
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <type_traits>

using namespace galay::kernel;

namespace {

constexpr std::array kReasons{
    CompletionReason::kReady, CompletionReason::kBackendError,
    CompletionReason::kTimedOut, CompletionReason::kCancelled,
    CompletionReason::kResourceClosed, CompletionReason::kRuntimeStopped};

void testTransitions()
{
    static_assert(!std::is_copy_constructible_v<OperationState>);
    static_assert(!std::is_move_constructible_v<OperationState>);
    OperationState local;
    assert(!local.key().isValid());
    assert(!local.markSubmitted());
    assert(!local.completionReason());
    assert(!local.markResumeIssued());
    auto underflow = local.releasePhysicalReference();
    assert(!underflow && underflow.error() == OperationError::kNoPhysicalReference);

    OperationState op({7, 11});
    assert(op.key() == (OperationKey{7, 11}));
    assert(op.markSubmitted());
    assert(!op.markSubmitted());
    assert(op.requestCancel());
    assert(!op.requestCancel());
    assert(!op.completionReason()); // 请求本身不是完成。
    assert(op.tryComplete(CompletionReason::kCancelled));
    auto late = op.addPhysicalReference();
    assert(!late && late.error() == OperationError::kAlreadyCompleted);
    assert(op.physicalReferenceCount() == 0);
    assert(op.phase() == OperationPhase::kSafeToResume);
    assert(op.markResumeIssued());
    assert(!op.markResumeIssued());
    assert(op.phase() == OperationPhase::kResumeIssued);
    assert(!op.requestCancel());
    assert(!op.tryComplete(CompletionReason::kReady));
    assert(op.completionReason() == CompletionReason::kCancelled);
}

void testEveryWinnerAndLoser()
{
    for (auto first : kReasons) {
        for (auto second : kReasons) {
            OperationState op({1, 1});
            assert(op.tryComplete(first)); // Created 也可同步失败/完成。
            assert(!op.tryComplete(second));
            assert(op.completionReason() == first);
            assert(op.phase() == OperationPhase::kSafeToResume);
        }
    }
}

void testCompletionAndDrainOrders()
{
    // 两个完成候选、两个 backend 回执的全部 24 种 owner 消费顺序。
    std::array events{0, 1, 2, 3};
    unsigned permutations = 0;
    do {
        OperationState op({3, 9});
        assert(op.markSubmitted());
        assert(op.addPhysicalReference());
        assert(op.addPhysicalReference());
        unsigned winners = 0;
        unsigned safe_transitions = 0;
        for (int event : events) {
            const bool was_safe = op.phase() == OperationPhase::kSafeToResume;
            if (event < 2) {
                winners += op.tryComplete(event == 0 ? CompletionReason::kReady
                                                     : CompletionReason::kTimedOut);
            } else {
                auto released = op.releasePhysicalReference();
                assert(released);
                assert(*released == (!was_safe && op.phase() == OperationPhase::kSafeToResume));
            }
            if (op.completionReason()) {
                auto late = op.addPhysicalReference();
                assert(!late && late.error() == OperationError::kAlreadyCompleted);
            }
            safe_transitions += !was_safe && op.phase() == OperationPhase::kSafeToResume;
        }
        assert(winners == 1 && safe_transitions == 1);
        assert(op.physicalReferenceCount() == 0);
        assert(op.markResumeIssued());
        ++permutations;
    } while (std::next_permutation(events.begin(), events.end()));
    assert(permutations == 24);
}

} // namespace

int main()
{
    testTransitions();
    testEveryWinnerAndLoser();
    testCompletionAndDrainOrders();
    std::cout << "T187-OperationStateCompletion PASS (36 winner pairs, 24 drain orders)\n";
}
