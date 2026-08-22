/**
 * @file t148_custom_awaitable.cc
 * @brief 验证模板策略式自定义 awaitable：显式 timeout policy、CRTP 转发和惰性 timer。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>

#include <cassert>
#include <chrono>
#include <expected>
#include <utility>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using Result = std::expected<int, IOError>;

struct CustomAwaitable;

/**
 * A policy is stateless and selected in the type.  The timeout wrapper can
 * inline both calls; no virtual function or type-erased callback is needed.
 */
struct CustomTimeoutPolicy {
    static void inject(CustomAwaitable& awaitable) noexcept;

    static bool ownsIoRegistration(CustomAwaitable&) noexcept {
        return true;
    }
};

struct CustomAwaitable : TimeoutSupport<CustomAwaitable, CustomTimeoutPolicy> {
    explicit CustomAwaitable(bool ready, int value = 7) noexcept
        : m_ready(ready), m_result(value) {}

    bool await_ready() const noexcept { return m_ready; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise>) noexcept {
        return false;
    }

    Result await_resume() noexcept {
        return std::move(m_result);
    }

    void setTimeout() noexcept {
        m_result = std::unexpected(IOError(kTimeout, 0));
    }

private:
    bool m_ready = false;
    Result m_result;
};

void CustomTimeoutPolicy::inject(CustomAwaitable& awaitable) noexcept {
    awaitable.setTimeout();
}

struct ForwardedAwaitable
    : ForwardingAwaitable<ForwardedAwaitable, CustomAwaitable> {
    using Base = ForwardingAwaitable<ForwardedAwaitable, CustomAwaitable>;

    explicit ForwardedAwaitable(CustomAwaitable inner) noexcept
        : Base(std::move(inner)) {}
};

void checkReadyPathDoesNotCreateTimer() {
    auto wrapped = CustomAwaitable(true).timeout(5ms);
    assert(wrapped.await_ready());
    assert(!wrapped.m_timer);
    auto result = wrapped.await_resume();
    assert(result.has_value() && result.value() == 7);
}

void checkTimeoutPolicyIsApplied() {
    auto wrapped = CustomAwaitable(false).timeout(5ms);
    wrapped.ensureTimer();
    wrapped.m_timer->handleTimeout();
    auto result = wrapped.await_resume();
    assert(!result.has_value());
    assert(IOError::contains(result.error().code(), kTimeout));
}

void checkForwardingSurface() {
    static_assert(concepts::Awaitable<ForwardedAwaitable>);
    ForwardedAwaitable facade(CustomAwaitable(true));
    assert(facade.await_ready());
    auto result = facade.await_resume();
    assert(result.has_value() && result.value() == 7);
}

}  // namespace

int main() {
    checkReadyPathDoesNotCreateTimer();
    checkTimeoutPolicyIsApplied();
    checkForwardingSurface();
    return 0;
}
