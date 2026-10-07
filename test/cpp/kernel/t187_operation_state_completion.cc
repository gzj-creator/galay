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

void test_transitions()
{
    static_assert(!std::is_copy_constructible_v<OperationState>);
    static_assert(!std::is_move_constructible_v<OperationState>);
    OperationState local;
    assert(!local.key().is_valid());
    assert(!local.mark_submitted());
    assert(!local.completion_reason());
    assert(!local.mark_resume_issued());
    auto underflow = local.release_physical_reference();
    assert(!underflow && underflow.error() == OperationError::kNoPhysicalReference);

    OperationState op({7, 11});
    assert(op.key() == (OperationKey{7, 11}));
    assert(op.mark_submitted());
    assert(!op.mark_submitted());
    assert(op.request_cancel());
    assert(!op.request_cancel());
    assert(!op.completion_reason()); // 请求本身不是完成。
    assert(op.try_complete(CompletionReason::kCancelled));
    auto late = op.add_physical_reference();
    assert(!late && late.error() == OperationError::kAlreadyCompleted);
    assert(op.physical_reference_count() == 0);
    assert(op.phase() == OperationPhase::kSafeToResume);
    assert(op.mark_resume_issued());
    assert(!op.mark_resume_issued());
    assert(op.phase() == OperationPhase::kResumeIssued);
    assert(!op.request_cancel());
    assert(!op.try_complete(CompletionReason::kReady));
    assert(op.completion_reason() == CompletionReason::kCancelled);
}

void test_every_winner_and_loser()
{
    for (auto first : kReasons) {
        for (auto second : kReasons) {
            OperationState op({1, 1});
            assert(op.try_complete(first)); // Created 也可同步失败/完成。
            assert(!op.try_complete(second));
            assert(op.completion_reason() == first);
            assert(op.phase() == OperationPhase::kSafeToResume);
        }
    }
}

void test_completion_and_drain_orders()
{
    // 两个完成候选、两个 backend 回执的全部 24 种 owner 消费顺序。
    std::array events{0, 1, 2, 3};
    unsigned permutations = 0;
    do {
        OperationState op({3, 9});
        assert(op.mark_submitted());
        assert(op.add_physical_reference());
        assert(op.add_physical_reference());
        unsigned winners = 0;
        unsigned safe_transitions = 0;
        for (int event : events) {
            const bool was_safe = op.phase() == OperationPhase::kSafeToResume;
            if (event < 2) {
                winners += op.try_complete(event == 0 ? CompletionReason::kReady
                                                     : CompletionReason::kTimedOut);
            } else {
                auto released = op.release_physical_reference();
                assert(released);
                assert(*released == (!was_safe && op.phase() == OperationPhase::kSafeToResume));
            }
            if (op.completion_reason()) {
                auto late = op.add_physical_reference();
                assert(!late && late.error() == OperationError::kAlreadyCompleted);
            }
            safe_transitions += !was_safe && op.phase() == OperationPhase::kSafeToResume;
        }
        assert(winners == 1 && safe_transitions == 1);
        assert(op.physical_reference_count() == 0);
        assert(op.mark_resume_issued());
        ++permutations;
    } while (std::next_permutation(events.begin(), events.end()));
    assert(permutations == 24);
}

} // namespace

int main()
{
    test_transitions();
    test_every_winner_and_loser();
    test_completion_and_drain_orders();
    std::cout << "T187-OperationStateCompletion PASS (36 winner pairs, 24 drain orders)\n";
}
