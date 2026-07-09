/**
 * @file t8_move_only_state_contract.cc
 * @brief 用途：锁定 SSL 状态机、awaitable 与 operation driver 的 move-only 所有权契约。
 * 关键覆盖点：TLS/IO/state-machine 状态不可隐式 copy；可安全转移的 driver/linear machine
 * 显式 noexcept move；状态机 awaitable 可在挂起前移动以支持 `.timeout(...)`；
 * 薄配置和错误值仍保持可 copy。
 * 通过条件：目标成功编译，静态断言成立，测试返回 0。
 */

#include <galay/cpp/galay-ssl/async/awaitable.h>
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <array>
#include <expected>
#include <type_traits>

using namespace galay::ssl;
using namespace galay::kernel;

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

struct SurfaceFlow {
    std::array<char, 8> scratch{};
    std::array<char, 4> reply{'p', 'o', 'n', 'g'};

    void onHandshake(SslBuilderOps<SurfaceResult, 8>&, SslHandshakeContext&) {}
    void onRecv(SslBuilderOps<SurfaceResult, 8>&, SslRecvContext&) {}
    ParseStatus onParse(SslBuilderOps<SurfaceResult, 8>&) { return ParseStatus::kCompleted; }
    void onSend(SslBuilderOps<SurfaceResult, 8>&, SslSendContext&) {}
    void onShutdown(SslBuilderOps<SurfaceResult, 8>&, SslShutdownContext&) {}
    void onFinish(SslBuilderOps<SurfaceResult, 8>& ops) { ops.complete(SurfaceResult{0}); }
};

using AwaitableT = SslStateMachineAwaitable<SurfaceMachine>;
using LinearMachineT = galay::ssl::detail::SslLinearMachine<SurfaceResult, 8, SurfaceFlow>;

static_assert(!std::is_copy_constructible_v<SslOperationDriver>,
              "SslOperationDriver owns IO progress state and must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<SslOperationDriver>,
              "SslOperationDriver owns IO progress state and must not be copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<SslOperationDriver>,
              "SslOperationDriver should transfer ownership with noexcept move construction");
static_assert(std::is_nothrow_move_assignable_v<SslOperationDriver>,
              "SslOperationDriver should transfer ownership with noexcept move assignment");

static_assert(!std::is_copy_constructible_v<AwaitableT>,
              "SslStateMachineAwaitable owns coroutine/IO state and must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<AwaitableT>,
              "SslStateMachineAwaitable owns coroutine/IO state and must not be copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<AwaitableT>,
              "SslStateMachineAwaitable must move into timeout wrappers before suspension");
static_assert(std::is_nothrow_move_assignable_v<AwaitableT>,
              "SslStateMachineAwaitable must move-assign before suspension");

static_assert(!std::is_copy_constructible_v<SslRecvAwaitable>);
static_assert(!std::is_copy_constructible_v<SslSendAwaitable>);
static_assert(!std::is_copy_constructible_v<SslHandshakeAwaitable>);
static_assert(!std::is_copy_constructible_v<SslShutdownAwaitable>);

static_assert(!std::is_copy_constructible_v<LinearMachineT>,
              "SslLinearMachine owns queued state-machine nodes and must not be copy-constructible");
static_assert(!std::is_copy_assignable_v<LinearMachineT>,
              "SslLinearMachine owns queued state-machine nodes and must not be copy-assignable");
static_assert(std::is_nothrow_move_constructible_v<LinearMachineT>,
              "SslLinearMachine should transfer queued state with noexcept move construction");
static_assert(std::is_nothrow_move_assignable_v<LinearMachineT>,
              "SslLinearMachine should transfer queued state with noexcept move assignment");

static_assert(!std::is_copy_constructible_v<galay::ssl::detail::SslSingleHandshakeMachine>);
static_assert(!std::is_copy_constructible_v<galay::ssl::detail::SslSingleRecvMachine>);
static_assert(!std::is_copy_constructible_v<galay::ssl::detail::SslSingleSendMachine>);
static_assert(!std::is_copy_constructible_v<galay::ssl::detail::SslSingleShutdownMachine>);
static_assert(std::is_nothrow_move_constructible_v<galay::ssl::detail::SslSingleHandshakeMachine>);
static_assert(std::is_nothrow_move_constructible_v<galay::ssl::detail::SslSingleRecvMachine>);
static_assert(std::is_nothrow_move_constructible_v<galay::ssl::detail::SslSingleSendMachine>);
static_assert(std::is_nothrow_move_constructible_v<galay::ssl::detail::SslSingleShutdownMachine>);

static_assert(!std::is_copy_constructible_v<SslSocket>);
static_assert(std::is_nothrow_move_constructible_v<SslSocket>);
static_assert(!std::is_copy_constructible_v<SslContext>);
static_assert(std::is_nothrow_move_constructible_v<SslContext>);

static_assert(std::is_copy_constructible_v<SslError>);
static_assert(std::is_copy_assignable_v<SslError>);
static_assert(std::is_trivially_copyable_v<SslMethod>);
static_assert(std::is_trivially_copyable_v<SslVerifyMode>);
static_assert(std::is_copy_constructible_v<SslMachineAction<SurfaceResult>>);

int main()
{
    return 0;
}
