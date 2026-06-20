/**
 * @file t7_await.cc
 * @brief 用途：锁定 sync/async etcd client 的公开 API surface。
 * 关键覆盖点：
 * - Async 内部 awaitable 辅助类型不对外暴露
 * - sync/async client 不再公开任何 `last*` 访问器
 * - sync/async 操作直接返回结构化结果
 * - Async client 暴露 `watch(key, Task)` 与 `watch(key, function)` 两个重载
 * 通过条件：目标成功编译，静态断言成立，程序返回 0。
 */

#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/sync/etcd_client.h>

#include <concepts>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

using galay::etcd::AsyncEtcdClient;
using galay::etcd::EtcdClient;
using galay::etcd::PipelineOp;

namespace {

template <typename ClientT>
concept HasConnectIoAwaitable = requires {
    typename ClientT::ConnectIoAwaitable;
};

template <typename ClientT>
concept HasCloseIoAwaitable = requires {
    typename ClientT::CloseIoAwaitable;
};

template <typename ClientT>
concept HasHttpPostAwaitable = requires {
    typename ClientT::HttpPostAwaitable;
};

template <typename ClientT>
concept HasIoAwaitableBase = requires {
    typename ClientT::template IoAwaitableBase<int>;
};

template <typename ClientT>
concept HasJsonOpAwaitableBase = requires {
    typename ClientT::JsonOpAwaitableBase;
};

template <typename ClientT>
concept HasLastError = requires(ClientT& client) {
    client.lastError();
};

template <typename ClientT>
concept HasLastBool = requires(ClientT& client) {
    client.lastBool();
};

template <typename ClientT>
concept HasLastLeaseId = requires(ClientT& client) {
    client.lastLeaseId();
};

template <typename ClientT>
concept HasLastDeletedCount = requires(ClientT& client) {
    client.lastDeletedCount();
};

template <typename ClientT>
concept HasLastKeyValues = requires(ClientT& client) {
    client.lastKeyValues();
};

template <typename ClientT>
concept HasLastPipelineResults = requires(ClientT& client) {
    client.lastPipelineResults();
};

template <typename ClientT>
concept HasLastStatusCode = requires(ClientT& client) {
    client.lastStatusCode();
};

template <typename ClientT>
concept HasLastResponseBody = requires(ClientT& client) {
    client.lastResponseBody();
};

template <typename ClientT>
concept SyncClientReturnsDirectResults = requires(
    ClientT& client,
    const std::string& key,
    const std::string& value,
    int64_t lease_id,
    std::vector<PipelineOp> ops) {
    { client.connect() } -> std::same_as<galay::etcd::EtcdBoolResult>;
    { client.close() } -> std::same_as<galay::etcd::EtcdBoolResult>;
    { client.put(key, value) } -> std::same_as<galay::etcd::EtcdBoolResult>;
    { client.put(key, value, lease_id) } -> std::same_as<galay::etcd::EtcdBoolResult>;
    { client.get(key) } -> std::same_as<galay::etcd::EtcdGetResult>;
    { client.del(key) } -> std::same_as<galay::etcd::EtcdDeleteResult>;
    { client.grantLease(3) } -> std::same_as<galay::etcd::EtcdLeaseGrantResult>;
    { client.keepAliveOnce(lease_id) } -> std::same_as<galay::etcd::EtcdLeaseGrantResult>;
    { client.pipeline(ops) } -> std::same_as<galay::etcd::EtcdPipelineResult>;
};

template <typename ClientT>
concept AsyncClientReturnsDirectResults =
    std::same_as<typename ClientT::ConnectAwaitable::Result, galay::etcd::EtcdBoolResult> &&
    std::same_as<decltype(std::declval<typename ClientT::CloseAwaitable&>().await_resume()), galay::etcd::EtcdBoolResult> &&
    std::same_as<decltype(std::declval<typename ClientT::PutAwaitable&>().await_resume()), galay::etcd::EtcdBoolResult> &&
    std::same_as<decltype(std::declval<typename ClientT::GetAwaitable&>().await_resume()), galay::etcd::EtcdGetResult> &&
    std::same_as<decltype(std::declval<typename ClientT::DeleteAwaitable&>().await_resume()), galay::etcd::EtcdDeleteResult> &&
    std::same_as<decltype(std::declval<typename ClientT::GrantLeaseAwaitable&>().await_resume()), galay::etcd::EtcdLeaseGrantResult> &&
    std::same_as<decltype(std::declval<typename ClientT::KeepAliveAwaitable&>().await_resume()), galay::etcd::EtcdLeaseGrantResult> &&
    std::same_as<decltype(std::declval<typename ClientT::PipelineAwaitable&>().await_resume()), galay::etcd::EtcdPipelineResult>;

template <typename ClientT>
concept HasAsyncWatchOverloads = requires(ClientT& client, const std::string& key) {
    { client.watch(
        key,
        std::function<galay::kernel::Task<void>(galay::etcd::EtcdWatchResponse)>{}) } ->
        std::same_as<galay::etcd::EtcdBoolResult>;
    { client.watch(
        key,
        std::function<void(galay::etcd::EtcdWatchResponse)>{}) } ->
        std::same_as<galay::etcd::EtcdBoolResult>;
};

}  // namespace

static_assert(!HasConnectIoAwaitable<AsyncEtcdClient>);
static_assert(!HasCloseIoAwaitable<AsyncEtcdClient>);
static_assert(!HasHttpPostAwaitable<AsyncEtcdClient>);
static_assert(!HasIoAwaitableBase<AsyncEtcdClient>);
static_assert(!HasJsonOpAwaitableBase<AsyncEtcdClient>);

static_assert(!HasLastError<EtcdClient>);
static_assert(!HasLastBool<EtcdClient>);
static_assert(!HasLastLeaseId<EtcdClient>);
static_assert(!HasLastDeletedCount<EtcdClient>);
static_assert(!HasLastKeyValues<EtcdClient>);
static_assert(!HasLastPipelineResults<EtcdClient>);
static_assert(!HasLastStatusCode<EtcdClient>);
static_assert(!HasLastResponseBody<EtcdClient>);

static_assert(!HasLastError<AsyncEtcdClient>);
static_assert(!HasLastBool<AsyncEtcdClient>);
static_assert(!HasLastLeaseId<AsyncEtcdClient>);
static_assert(!HasLastDeletedCount<AsyncEtcdClient>);
static_assert(!HasLastKeyValues<AsyncEtcdClient>);
static_assert(!HasLastPipelineResults<AsyncEtcdClient>);
static_assert(!HasLastStatusCode<AsyncEtcdClient>);
static_assert(!HasLastResponseBody<AsyncEtcdClient>);

static_assert(SyncClientReturnsDirectResults<EtcdClient>);
static_assert(AsyncClientReturnsDirectResults<AsyncEtcdClient>);
static_assert(HasAsyncWatchOverloads<AsyncEtcdClient>);

int main()
{
    return 0;
}
