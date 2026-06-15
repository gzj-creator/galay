/**
 * @file t4_await.cpp
 * @brief 用途：锁定 RPC 仓库公开 awaitable 表面已切到 builder/state-machine 内核。
 * 关键覆盖点：`RpcConn` / `RpcStream` / `RpcClient` 的公开 awaitable facade
 * 不再直接暴露老的 `ReadvAwaitable` / `WritevAwaitable` / `ReadvIOContext` 继承关系。
 * 通过条件：目标成功编译，静态断言成立，程序返回 0。
 */

#include "galay-rpc/kernel/rpc_client.h"
#include "galay-rpc/kernel/rpc_conn.h"
#include "galay-rpc/kernel/rpc_stream.h"

#include <concepts>
#include <type_traits>

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::rpc;

static_assert(!std::derived_from<GetRpcRequestAwaitable<TcpSocket>, ReadvAwaitable>);
static_assert(!std::derived_from<GetRpcResponseAwaitable<TcpSocket>, ReadvAwaitable>);
static_assert(!std::derived_from<GetRpcHeaderAwaitable<TcpSocket>, ReadvAwaitable>);
static_assert(!std::derived_from<GetRpcBodyAwaitable<TcpSocket>, ReadvAwaitable>);

static_assert(!std::derived_from<SendRpcRequestAwaitable<TcpSocket>, WritevAwaitable>);
static_assert(!std::derived_from<SendRpcResponseAwaitable<TcpSocket>, WritevAwaitable>);
static_assert(!std::derived_from<SendRawDataAwaitable<TcpSocket>, WritevAwaitable>);
static_assert(!std::derived_from<SendStreamDataAwaitable<TcpSocket>, WritevAwaitable>);
static_assert(!std::derived_from<GetStreamMessageAwaitable<TcpSocket>, ReadvAwaitable>);

static_assert(!std::derived_from<RecvRpcResponseChainAwaitable<TcpSocket>, ReadvIOContext>);

int main()
{
    return 0;
}
