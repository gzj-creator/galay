/** @file t188_operation_completion_boundary.cc
 *  @brief 不依赖 controller/reactor 的 result + resume 所有权边界；不是 accept 集成验收。 */
#include <galay/cpp/galay-kernel/core/operation_completion.hpp>
#include <galay/cpp/galay-kernel/common/error.h>

#if defined(GALAY_KERNEL_IOCONTROLLER_HPP) || defined(GALAY_KERNEL_TIMEOUT_HPP) || \
    defined(GALAY_KERNEL_AWAITABLE_H)
#error "OperationCompletion must not depend on a resource/backend/timeout adapter"
#endif

#include <cassert>
#include <array>
#include <expected>
#include <iostream>
#include <memory>
#include <type_traits>

using namespace galay::kernel;

namespace {

struct Probe {
    detail::ResumeTokenHeader header;
    void* operation = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    unsigned wakes = 0;
};

const detail::ResumeTokenHooks kHooks{
    .owner_scheduler = [](void*) noexcept -> Scheduler* { return nullptr; },
    .request_resume = [](void* state) noexcept {
        auto& probe = *static_cast<Probe*>(state);
        ++probe.wakes;
        if (probe.destroy) {
            probe.destroy(probe.operation); // 模拟 C callback 内联恢复并销毁 awaiter。
        }
        return true;
    },
};

Waker makeWaker(Probe& probe)
{
    probe.header.hooks = &kHooks;
    return Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe));
}

void testResultCannotBeOverwritten()
{
    using Result = std::expected<int, IOError>;
    OperationCompletion<Result> op({1, 1});
    assert(op.markSubmitted());
    assert(op.tryComplete(CompletionReason::kResourceClosed,
                          Result(std::unexpected(IOError(kClosed, 0)))));
    assert(!op.tryComplete(CompletionReason::kTimedOut,
                           Result(std::unexpected(IOError(kTimeout, 0)))));
    auto resume = op.takeResume();
    assert(resume); // 同步完成允许空 Waker，取出恢复权仍然只有一次。
    auto result = op.takeResult();
    assert(result && !*result);
    assert((result->error().code() & 0xffffffffu) == kClosed);
    auto duplicate = op.takeResult();
    assert(!duplicate && duplicate.error() == OperationError::kResultUnavailable);
}

void testDrainAndMoveOnlyPayload()
{
    using Completion = OperationCompletion<std::unique_ptr<int>>;
    static_assert(!std::is_copy_constructible_v<Completion>);
    static_assert(!std::is_move_constructible_v<Completion>);
    Probe probe{};
    Completion op({2, 1}, makeWaker(probe));
    assert(op.markSubmitted());
    assert(op.addPhysicalReference());
    assert(op.addPhysicalReference());
    auto early = op.takeResume();
    assert(!early && early.error() == OperationError::kNotSafeToResume);
    auto pending_result = op.takeResult();
    assert(!pending_result && pending_result.error() == OperationError::kResultUnavailable);
    auto value = std::make_unique<int>(42);
    assert(op.tryComplete(CompletionReason::kReady, std::move(value)));
    assert(!value);
    auto loser = std::make_unique<int>(99);
    assert(!op.tryComplete(CompletionReason::kTimedOut, std::move(loser)));
    assert(loser && *loser == 99); // 败者必须仍拥有自己的 payload/资源。
    assert(op.state().completionReason() == CompletionReason::kReady);
    early = op.takeResume();
    assert(!early && probe.wakes == 0);
    auto released = op.releasePhysicalReference();
    assert(released && !*released);
    released = op.releasePhysicalReference();
    assert(released && *released);
    auto resume = op.takeResume();
    assert(resume);
    auto duplicate = op.takeResume();
    assert(!duplicate && duplicate.error() == OperationError::kResumeAlreadyTaken);
    std::move(*resume).resume();
    std::move(*resume).resume(); // 已消费的 capability 不得重复唤醒。
    assert(probe.wakes == 1);
    auto result = op.takeResult();
    assert(result && **result == 42);
}

void testInlineDestructionAfterHandoff()
{
    using Completion = OperationCompletion<int>;
    Probe probe{};
    auto op = std::make_unique<Completion>(OperationKey{3, 1}, makeWaker(probe));
    probe.operation = &op;
    probe.destroy = [](void* owner) noexcept {
        static_cast<std::unique_ptr<Completion>*>(owner)->reset();
    };
    assert(op->tryComplete(CompletionReason::kReady, 7));
    auto resume = op->takeResume();
    assert(resume);
    std::move(*resume).resume(); // 之后只访问独立 probe，不再读取 operation/frame。
    assert(!op && probe.wakes == 1);
}

void testEveryResultAndResumeWinner()
{
    static_assert(!std::is_copy_constructible_v<ResumeCapability>);
    static_assert(std::is_nothrow_move_constructible_v<ResumeCapability>);
    static_assert(!std::is_constructible_v<ResumeCapability, Waker&>);
    static_assert(!std::is_constructible_v<OperationCompletion<int>, OperationKey, Waker&>);
    constexpr std::array reasons{
        CompletionReason::kReady, CompletionReason::kBackendError,
        CompletionReason::kTimedOut, CompletionReason::kCancelled,
        CompletionReason::kResourceClosed, CompletionReason::kRuntimeStopped};
    for (auto first : reasons) {
        for (auto second : reasons) {
            Probe probe{};
            OperationCompletion<int> op({1, 1}, makeWaker(probe));
            assert(op.tryComplete(first, 42));
            assert(!op.tryComplete(second, 99));
            auto resume = op.takeResume();
            assert(resume);
            std::move(*resume).resume();
            assert(probe.wakes == 1 && op.state().completionReason() == first);
            auto result = op.takeResult();
            assert(result && *result == 42);
        }
    }
    OperationCompletion<std::expected<void, IOError>> void_op;
    assert(void_op.tryComplete(CompletionReason::kReady, {}));
    auto resume = void_op.takeResume();
    assert(resume);
    auto result = void_op.takeResult();
    assert(result && result->has_value());
}

} // namespace

int main()
{
    testResultCannotBeOverwritten();
    testDrainAndMoveOnlyPayload();
    testInlineDestructionAfterHandoff();
    testEveryResultAndResumeWinner();
    std::cout << "T188-OperationCompletionBoundary PASS\n";
}
