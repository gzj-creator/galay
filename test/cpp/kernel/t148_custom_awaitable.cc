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

    static bool owns_io_registration(CustomAwaitable&) noexcept {
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

    void set_timeout() noexcept {
        m_result = std::unexpected(IOError(kTimeout, 0));
    }

private:
    bool m_ready = false;
    Result m_result;
};

void CustomTimeoutPolicy::inject(CustomAwaitable& awaitable) noexcept {
    awaitable.set_timeout();
}

struct ForwardedAwaitable
    : ForwardingAwaitable<ForwardedAwaitable, CustomAwaitable> {
    using Base = ForwardingAwaitable<ForwardedAwaitable, CustomAwaitable>;

    explicit ForwardedAwaitable(CustomAwaitable inner) noexcept
        : Base(std::move(inner)) {}
};

void check_ready_path_does_not_create_timer() {
    auto wrapped = CustomAwaitable(true).timeout(5ms);
    assert(wrapped.await_ready());
    assert(!wrapped.m_timer);
    auto result = wrapped.await_resume();
    assert(result.has_value() && result.value() == 7);
}

void check_timeout_policy_is_applied() {
    auto wrapped = CustomAwaitable(false).timeout(5ms);
    wrapped.ensure_timer();
    wrapped.m_timer->handle_timeout();
    auto result = wrapped.await_resume();
    assert(!result.has_value());
    assert(IOError::contains(result.error().code(), kTimeout));
}

void check_forwarding_surface() {
    static_assert(concepts::Awaitable<ForwardedAwaitable>);
    ForwardedAwaitable facade(CustomAwaitable(true));
    assert(facade.await_ready());
    auto result = facade.await_resume();
    assert(result.has_value() && result.value() == 7);
}

}  // namespace

int main() {
    check_ready_path_does_not_create_timer();
    check_timeout_policy_is_applied();
    check_forwarding_surface();
    return 0;
}
