/**
 * @file t149_timeout_policy_surface.cc
 * @brief 锁定 timeout policy 的编译期契约与 ownsIoRegistration 定制点。
 */

#include <galay/cpp/galay-kernel/core/timeout.hpp>

#include <cassert>
#include <expected>
#include <utility>

using namespace galay::kernel;

namespace {

struct NoTimeoutAwaitable {
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise>) noexcept { return false; }

    int await_resume() const noexcept { return 1; }
};

static_assert(!concepts::TimeoutInjectable<NoTimeoutAwaitable>);

struct ExistingResultAwaitable
    : TimeoutSupport<ExistingResultAwaitable> {
    using result_type = std::expected<int, IOError>;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise>) noexcept { return false; }

    result_type await_resume() noexcept { return std::move(m_result); }

    void setTimeout() noexcept {
        m_result = std::unexpected(IOError(kTimeout, 0));
    }

    bool ownsIoRegistration() const noexcept { return false; }

private:
    result_type m_result{42};
};

void checkExplicitOwnershipHookWins() {
    auto wrapped = ExistingResultAwaitable{}.timeout(std::chrono::milliseconds(1));
    wrapped.ensureTimer();
    wrapped.m_timer->handleTimeout();

    auto result = wrapped.await_resume();
    assert(result.has_value());
    assert(result.value() == 42);
}

}  // namespace

int main() {
    checkExplicitOwnershipHookWins();
    return 0;
}
