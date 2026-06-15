/**
 * @file t62_then.cc
 * @brief 用途：锁定 `Task<void>::then(...)` 作为根任务 continuation 公开接口的存在性。
 * 关键覆盖点：`task.h` 入口、`Task<void>` 链式 then、左值/右值 then 返回类型。
 * 通过条件：编译期静态断言与运行期链式调用全部通过，测试返回 0。
 */

#include "galay-kernel/core/task.h"

#include <concepts>
#include <type_traits>
#include <utility>

using namespace galay::kernel;

Task<void> pushStep(int)
{
    co_return;
}

template <typename T>
concept HasTaskThenLvalue = requires(T left, T right) {
    { left.then(std::move(right)) } -> std::same_as<T&>;
};

template <typename T>
concept HasTaskThenRvalue = requires(T left, T right) {
    { std::move(left).then(std::move(right)) } -> std::same_as<T&&>;
};

static_assert(HasTaskThenLvalue<Task<void>>);
static_assert(HasTaskThenRvalue<Task<void>>);

int main()
{
    [[maybe_unused]] auto root = pushStep(1).then(pushStep(2));
    return 0;
}
