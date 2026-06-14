/**
 * @file t68_smawait.cc
 * @brief 用途：锁定 Awaitable 状态机公开表面与 builder 状态机入口。
 * 关键覆盖点：`MachineSignal`、`MachineAction`、`AwaitableStateMachine`、
 * `StateMachineAwaitable`、`AwaitableBuilder::fromStateMachine(...)`。
 * 通过条件：目标成功编译，静态断言成立，测试返回 0。
 */

#include "kernel/kernel/awaitable.h"
#include <concepts>
#include <expected>
#include <type_traits>

using namespace galay::kernel;

using SurfaceResult = std::expected<size_t, IOError>;

struct SurfaceMachine {
    using result_type = SurfaceResult;

    MachineAction<result_type> advance() {
        return MachineAction<result_type>::complete(result_type{0});
    }

    void onRead(std::expected<size_t, IOError>) {}
    void onWrite(std::expected<size_t, IOError>) {}
};

template <typename BuilderT>
concept HasFromStateMachine = requires(IOController* controller, SurfaceMachine machine) {
    { BuilderT::fromStateMachine(controller, std::move(machine)) };
};

static_assert(std::same_as<decltype(MachineSignal::kContinue), MachineSignal>);
static_assert(std::same_as<decltype(MachineSignal::kWaitConnect), MachineSignal>);
static_assert(std::is_same_v<decltype(MachineAction<SurfaceResult>::continue_()), MachineAction<SurfaceResult>>);
static_assert(std::is_same_v<
              decltype(MachineAction<SurfaceResult>::waitConnect(Host{})),
              MachineAction<SurfaceResult>>);
static_assert(AwaitableStateMachine<SurfaceMachine>);
static_assert(std::constructible_from<StateMachineAwaitable<SurfaceMachine>, IOController*, SurfaceMachine>);
static_assert(HasFromStateMachine<AwaitableBuilder<SurfaceResult>>);

int main()
{
    return 0;
}
