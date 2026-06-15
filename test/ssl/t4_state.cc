/**
 * @file t4_state.cc
 * @brief 用途：锁定 SSL 状态机公开表面与 builder 状态机入口。
 * 关键覆盖点：`SslMachineAction`、`SslStateMachineAwaitable`、`SslAwaitableBuilder::fromStateMachine(...)`。
 * 通过条件：目标成功编译，静态断言成立，测试返回 0。
 */

#include "galay-ssl/async/awaitable.h"
#include <concepts>
#include <expected>
#include <type_traits>

using namespace galay::ssl;

using SurfaceResult = std::expected<size_t, SslError>;

struct SurfaceMachine {
    using result_type = SurfaceResult;

    SslMachineAction<result_type> advance()
    {
        return SslMachineAction<result_type>::complete(result_type{0});
    }

    void onHandshake(std::expected<void, SslError>) {}
    void onRecv(std::expected<Bytes, SslError>) {}
    void onSend(std::expected<size_t, SslError>) {}
    void onShutdown(std::expected<void, SslError>) {}
};

template <typename BuilderT>
concept HasFromStateMachine = requires(IOController* controller, SslSocket* socket, SurfaceMachine machine) {
    { BuilderT::fromStateMachine(controller, socket, std::move(machine)) };
};

static_assert(std::is_same_v<decltype(SslMachineAction<SurfaceResult>::continue_()), SslMachineAction<SurfaceResult>>);
static_assert(std::is_same_v<decltype(SslMachineAction<SurfaceResult>::complete(SurfaceResult{0})), SslMachineAction<SurfaceResult>>);
static_assert(std::same_as<decltype(std::declval<AwaitContext>().scheduler), Scheduler*>);
static_assert(std::constructible_from<SslStateMachineAwaitable<SurfaceMachine>, IOController*, SslSocket*, SurfaceMachine>);
static_assert(HasFromStateMachine<SslAwaitableBuilder<SurfaceResult>>);

int main()
{
    return 0;
}
