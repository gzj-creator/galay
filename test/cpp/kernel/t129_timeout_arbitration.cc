/**
 * @file t129_timeout_arbitration.cc
 * @brief 验证 WithTimeout 在 IO completion 与 timeout 竞态中的结果裁决。
 *
 * 关键覆盖点：
 * - IO completion 已移除 awaitable 注册后，即使 timer 随后触发，也不能把成功结果覆盖成 timeout。
 * - awaitable 仍处于 IO 注册状态时，timer 触发应注入 `kTimeout`。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <cassert>
#include <chrono>
#include <iostream>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

auto make_timed_recv(IOController& controller, char* buffer)
{
    return RecvAwaitable(&controller, buffer, 1).timeout(1ms);
}

void completion_wins_when_registration_was_removed()
{
    char buffer = 0;
    IOController controller(GHandle{.fd = -1});
    auto awaitable = make_timed_recv(controller, &buffer);
    awaitable.ensure_timer();

    assert(controller.fill_awaitable(RECV, &awaitable.m_inner));
    awaitable.m_inner.m_result = size_t{1};
    controller.remove_awaitable(RECV);

    awaitable.m_timer->handle_timeout();
    auto result = awaitable.await_resume();

    assert(result.has_value());
    assert(*result == 1);
}

void timeout_wins_while_registration_is_still_active()
{
    char buffer = 0;
    IOController controller(GHandle{.fd = -1});
    auto awaitable = make_timed_recv(controller, &buffer);
    awaitable.ensure_timer();

    assert(controller.fill_awaitable(RECV, &awaitable.m_inner));

    awaitable.m_timer->handle_timeout();
    auto result = awaitable.await_resume();

    assert(!result.has_value());
    assert(IOError::contains(result.error().code(), kTimeout));
}

}  // namespace

int main()
{
    completion_wins_when_registration_was_removed();
    timeout_wins_while_registration_is_still_active();

    std::cout << "T129-TimeoutArbitration PASS\n";
    return 0;
}
