/**
 * @file t89_awready.cc
 * @brief 用途：锁定 `AwaitableBuilder::ready(...)` 与 `ReadyAwaitable` 的公开表面。
 * 关键覆盖点：`ReadyAwaitable<ResultT>`、`AwaitableBuilder<ResultT>::ready(...)`、
 * `timeout(...)` 兼容性。
 * 通过条件：目标成功编译，静态断言成立，测试返回 0。
 */

#include "kernel/kernel/awaitable.h"

#include <chrono>
#include <concepts>
#include <expected>
#include <type_traits>

using namespace galay::kernel;
using namespace std::chrono_literals;

using ReadyResult = std::expected<int, IOError>;

template <typename BuilderT>
concept HasReadyFactory = requires {
    { BuilderT::ready(ReadyResult{7}) } -> std::same_as<ReadyAwaitable<ReadyResult>>;
};

template <typename AwaitableT>
concept HasTimeout = requires(AwaitableT awaitable) {
    { awaitable.timeout(1ms) };
};

static_assert(HasReadyFactory<AwaitableBuilder<ReadyResult>>);
static_assert(std::constructible_from<ReadyAwaitable<ReadyResult>, ReadyResult>);
static_assert(HasTimeout<ReadyAwaitable<ReadyResult>>);
static_assert(requires {
    { AwaitableBuilder<ReadyResult>::ready(ReadyResult{11}).timeout(1ms) };
});

int main()
{
    return 0;
}
