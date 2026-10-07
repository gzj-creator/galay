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

Waker make_waker(Probe& probe)
{
    probe.header.hooks = &kHooks;
    return Waker(detail::ResumeToken::from_non_owning_c_coroutine(&probe));
}

void test_result_cannot_be_overwritten()
{
    using Result = std::expected<int, IOError>;
    OperationCompletion<Result> op({1, 1});
    assert(op.mark_submitted());
    assert(op.try_complete(CompletionReason::kResourceClosed,
                          Result(std::unexpected(IOError(kClosed, 0)))));
    assert(!op.try_complete(CompletionReason::kTimedOut,
                           Result(std::unexpected(IOError(kTimeout, 0)))));
    auto resume = op.take_resume();
    assert(resume); // 同步完成允许空 Waker，取出恢复权仍然只有一次。
    auto result = op.take_result();
    assert(result && !*result);
    assert((result->error().code() & 0xffffffffu) == kClosed);
    auto duplicate = op.take_result();
    assert(!duplicate && duplicate.error() == OperationError::kResultUnavailable);
}

void test_drain_and_move_only_payload()
{
    using Completion = OperationCompletion<std::unique_ptr<int>>;
    static_assert(!std::is_copy_constructible_v<Completion>);
    static_assert(!std::is_move_constructible_v<Completion>);
    Probe probe{};
    Completion op({2, 1}, make_waker(probe));
    assert(op.mark_submitted());
    assert(op.add_physical_reference());
    assert(op.add_physical_reference());
    auto early = op.take_resume();
    assert(!early && early.error() == OperationError::kNotSafeToResume);
    auto pending_result = op.take_result();
    assert(!pending_result && pending_result.error() == OperationError::kResultUnavailable);
    auto value = std::make_unique<int>(42);
    assert(op.try_complete(CompletionReason::kReady, std::move(value)));
    assert(!value);
    auto loser = std::make_unique<int>(99);
    assert(!op.try_complete(CompletionReason::kTimedOut, std::move(loser)));
    assert(loser && *loser == 99); // 败者必须仍拥有自己的 payload/资源。
    assert(op.state().completion_reason() == CompletionReason::kReady);
    early = op.take_resume();
    assert(!early && probe.wakes == 0);
    auto released = op.release_physical_reference();
    assert(released && !*released);
    released = op.release_physical_reference();
    assert(released && *released);
    auto resume = op.take_resume();
    assert(resume);
    auto duplicate = op.take_resume();
    assert(!duplicate && duplicate.error() == OperationError::kResumeAlreadyTaken);
    std::move(*resume).resume();
    std::move(*resume).resume(); // 已消费的 capability 不得重复唤醒。
    assert(probe.wakes == 1);
    auto result = op.take_result();
    assert(result && **result == 42);
}

void test_inline_destruction_after_handoff()
{
    using Completion = OperationCompletion<int>;
    Probe probe{};
    auto op = std::make_unique<Completion>(OperationKey{3, 1}, make_waker(probe));
    probe.operation = &op;
    probe.destroy = [](void* owner) noexcept {
        static_cast<std::unique_ptr<Completion>*>(owner)->reset();
    };
    assert(op->try_complete(CompletionReason::kReady, 7));
    auto resume = op->take_resume();
    assert(resume);
    std::move(*resume).resume(); // 之后只访问独立 probe，不再读取 operation/frame。
    assert(!op && probe.wakes == 1);
}

void test_every_result_and_resume_winner()
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
            OperationCompletion<int> op({1, 1}, make_waker(probe));
            assert(op.try_complete(first, 42));
            assert(!op.try_complete(second, 99));
            auto resume = op.take_resume();
            assert(resume);
            std::move(*resume).resume();
            assert(probe.wakes == 1 && op.state().completion_reason() == first);
            auto result = op.take_result();
            assert(result && *result == 42);
        }
    }
    OperationCompletion<std::expected<void, IOError>> void_op;
    assert(void_op.try_complete(CompletionReason::kReady, {}));
    auto resume = void_op.take_resume();
    assert(resume);
    auto result = void_op.take_result();
    assert(result && result->has_value());
}

} // namespace

int main()
{
    test_result_cannot_be_overwritten();
    test_drain_and_move_only_payload();
    test_inline_destruction_after_handoff();
    test_every_result_and_resume_winner();
    std::cout << "T188-OperationCompletionBoundary PASS\n";
}
