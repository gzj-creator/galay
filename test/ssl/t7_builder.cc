/**
 * @file t7_builder.cc
 * @brief 用途：锁定 SSL AwaitableBuilder 的公开链式表面。
 * 关键覆盖点：`fromStateMachine()`、`handshake()`、`recv()`、`send()`、`shutdown()`、`finish()`。
 * 通过条件：静态断言成立，测试返回 0。
 */

#include "galay-ssl/async/awaitable.h"
#include <array>
#include <concepts>
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

template <typename BuilderT>
concept HasFromStateMachine = requires(IOController* controller, SslSocket* socket, SurfaceMachine machine) {
    { BuilderT::fromStateMachine(controller, socket, std::move(machine)) };
};

using ChainedAwaitableT = decltype(
    std::declval<SslAwaitableBuilder<SurfaceResult, 8, SurfaceFlow>&>()
        .handshake<&SurfaceFlow::onHandshake>()
        .recv<&SurfaceFlow::onRecv>(std::declval<char*>(), std::declval<size_t>())
        .parse<&SurfaceFlow::onParse>()
        .send<&SurfaceFlow::onSend>(std::declval<const char*>(), std::declval<size_t>())
        .shutdown<&SurfaceFlow::onShutdown>()
        .finish<&SurfaceFlow::onFinish>()
        .build()
);

static_assert(HasFromStateMachine<SslAwaitableBuilder<SurfaceResult>>);
static_assert(std::same_as<decltype(std::declval<AwaitContext>().scheduler), Scheduler*>);
static_assert(
    !std::derived_from<std::remove_cvref_t<ChainedAwaitableT>, SequenceAwaitable<SurfaceResult, 8>>,
    "Chained SslAwaitableBuilder::build() should bridge to the SSL state-machine core"
);

int main()
{
    return 0;
}
