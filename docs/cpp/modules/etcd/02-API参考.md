# 02-API参考

本页只记录当前安装包 / 公开头文件 / module 接口中的可见接口，并补充必要语义说明。

## 1. 公开头文件与导出接口

当前安装包会安装这些头文件：

- `galay-etcd/base/network_cfg.h`
- `galay-etcd/base/etcd_config.h`
- `galay-etcd/base/etcd_error.h`
- `galay-etcd/base/etcd_log.h`
- `galay-etcd/base/etcd_value.h`
- `galay-etcd/base/etcd_types.h`
- `galay-etcd/async/client.h`
- `galay-etcd/module/module_prelude.hpp`
- `src/cpp/galay-etcd/module/` 下的 module 接口文件
- `galay-etcd/sync/etcd_client.h`

安装面补充说明：

- `src/cpp/galay-etcd/base/etcd_internal.h` 是源码树内部 helper 头，不属于安装/export 契约
- `galay-etcd/base/etcd_log.h` 提供基于 `galay::kernel::BaseLogger` 的库级日志入口与 `ETCD_LOG_*` 埋点宏
- `galay-etcd/module/module_prelude.hpp` 是 module 接口的 global module fragment 支撑头，不是 header 模式下的首选入口
- `src/cpp/galay-etcd/module/` 下的 module 接口文件回答 module 模式下的公开导出边界

构建接口：

- 子目录 target：`etcd`
- 命名空间 alias：`galay::etcd`
- module facade target：启用 import 编译时由构建系统提供
- 安装后 `find_package` 名称：`etcd`
- C++ module：`galay.etcd`

`galay.etcd` 的 module 导出边界，以 `src/cpp/galay-etcd/module/` 下 module 接口文件中的 `export { ... }` 块为准：

- 会被 `import galay.etcd;` 直接导出的头：`etcd_config.h`、`etcd_error.h`、`etcd_log.h`、`etcd_value.h`、`etcd_types.h`、`network_cfg.h`、`client.h`、`etcd_client.h`
- `module_prelude.hpp` 虽然在 global module fragment 中 `#include` 了更多头，但它不是额外的导出清单

`galay::etcd::internal` helper 仅供源码树内部（例如 `test/T6`）使用。

## 2. 基础类型与可选日志辅助

### `EtcdNetworkConfig`

```cpp
struct EtcdNetworkConfig {
    std::chrono::milliseconds request_timeout = std::chrono::milliseconds(-1);
    size_t buffer_size = 16384;
    bool keepalive = true;

    bool isRequestTimeoutEnabled() const;
    static EtcdNetworkConfig withTimeout(std::chrono::milliseconds timeout);
};
```

语义：

- `request_timeout < 0` 表示禁用
- `keepalive` 控制传输层 keep-alive，不是租约续约

### `EtcdConfig`

```cpp
struct EtcdConfig : EtcdNetworkConfig {
    std::string endpoint = "http://127.0.0.1:2379";
    std::string api_prefix = "/v3";
    EtcdProductionConfig production;
    EtcdCredentialConfig credentials;

    static EtcdConfig withTimeout(std::chrono::milliseconds timeout);
};
```

补充语义：

- `endpoint` 仍是当前 sync/async client 的实际请求地址
- `production.endpoints` 为后续 cluster wrapper 预留；builder 收到非空 endpoints 时，会把首个地址同步回 `endpoint`，保持既有单端点行为不变
- `credentials` 当前只提供配置承载与脱敏输出，不会自动启用 auth 流程

### `EtcdEndpointPolicy` / `EtcdRetryDecision`

```cpp
enum class EtcdEndpointPolicy {
    FirstHealthy,
    RoundRobin,
    StickyLeader,
};

enum class EtcdRetryDecision {
    RetrySameEndpoint,
    RetryNextEndpoint,
    FailFast,
};
```

### `EtcdRetryConfig`

```cpp
struct EtcdRetryConfig {
    size_t attempts = 3;
    std::chrono::milliseconds initial_backoff{25};
    std::chrono::milliseconds max_backoff{500};
    bool jitter = true;
};
```

### `EtcdCredentialConfig`

```cpp
struct EtcdCredentialConfig {
    std::string username;
    std::string password;
    std::string bearer_token;

    std::string redactedString() const;
};
```

语义：

- `redactedString()` 可用于日志/调试输出，但不会返回明文 `password` 或 `bearer_token`
- 当前模块没有自动登录或 token 刷新行为；这部分属于后续 auth task

### `EtcdProductionConfig`

```cpp
struct EtcdProductionConfig {
    std::vector<std::string> endpoints;
    EtcdEndpointPolicy endpoint_policy = EtcdEndpointPolicy::FirstHealthy;
    EtcdRetryConfig retry;
    std::chrono::milliseconds health_interval{5000};
    bool prefer_leader = false;
};
```

语义：

- `endpoints` 是 cluster policy 可选择的候选 endpoint 列表
- `prefer_leader` 当前只影响 `StickyLeader` 下的“最近一次成功端点” hint；不是独立的 leader 探测机制

### `EtcdKeyValue`

```cpp
struct EtcdKeyValue {
    std::string key;
    std::string value;
    int64_t create_revision = 0;
    int64_t mod_revision = 0;
    int64_t version = 0;
    int64_t lease = 0;
};
```

### `PipelineOpType`

```cpp
enum class PipelineOpType {
    Put,
    Get,
    Delete,
};
```

### `PipelineOp`

```cpp
struct PipelineOp {
    PipelineOpType type = PipelineOpType::Put;
    std::string key;
    std::string value;
    bool prefix = false;
    std::optional<int64_t> limit = std::nullopt;
    std::optional<int64_t> lease_id = std::nullopt;

    static PipelineOp Put(std::string key,
                          std::string value,
                          std::optional<int64_t> lease_id = std::nullopt);

    static PipelineOp Get(std::string key,
                          bool prefix = false,
                          std::optional<int64_t> limit = std::nullopt);

    static PipelineOp Del(std::string key, bool prefix = false);
};
```

### `PipelineItemResult`

```cpp
struct PipelineItemResult {
    PipelineOpType type = PipelineOpType::Put;
    bool ok = false;
    int64_t deleted_count = 0;
    std::vector<EtcdKeyValue> kvs;
};
```

### `EtcdClientStats`

```cpp
struct EtcdClientStats {
    uint64_t requests = 0;
    uint64_t request_failures = 0;
    uint64_t retries = 0;
    uint64_t endpoint_switches = 0;
    uint64_t auth_refreshes = 0;
    uint64_t watch_reconnects = 0;
    uint64_t watch_compactions = 0;
    uint64_t lease_keepalive_successes = 0;
    uint64_t lease_keepalive_failures = 0;
};
```

语义：

- `EtcdClient::getStats()` / `AsyncEtcdClient::getStats()` 当前返回只读快照
- `AsyncEtcdClusterClient::getStats()` 返回 offline policy loop 的统计快照，不代表真实网络 I/O 已接入
- 在本 task 完成后，普通单端点 client 默认仍返回零值；真实计数由后续生产 wrapper 接入

### `EtcdErrorType` 与 `EtcdError`

```cpp
enum class EtcdErrorType {
    Success = 0,
    InvalidEndpoint,
    InvalidParam,
    NotConnected,
    Connection,
    Timeout,
    Send,
    Recv,
    Http,
    Server,
    Parse,
    Internal,
};

class EtcdError {
public:
    EtcdError(EtcdErrorType type = EtcdErrorType::Success);
    EtcdError(EtcdErrorType type, std::string extra_msg);

    EtcdErrorType type() const;
    std::string message() const;
    bool isOk() const;
};
```

### `galay::etcd::log`

```cpp
namespace galay::etcd::log {
void set(::galay::kernel::BaseLogger::uptr logger);
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
}
```

方法语义：

- `set(logger)` 设置 etcd 模块的库级 logger；传入 `nullptr` 等价于禁用日志。
- `get()` 返回当前 logger 裸指针；生命周期由 `set()` 传入的 `std::unique_ptr` 管理，调用方不得释放。
- 该入口只影响 etcd 模块内部 `ETCD_LOG_*` 埋点，不会启用 `galay-kernel`、`galay-http` 或其他库的日志。
- `set()` 应在创建 client 前的单线程初始化阶段调用；运行期替换 logger 时调用方需要自行保证同步。
- 未设置 logger 或日志级别被过滤时，埋点不会执行 `std::format`，也不会求值格式化参数表达式。

日志宏：

```cpp
ETCD_LOG_ENABLED(level)
ETCD_LOG_TRACE(tag, fmt, ...)
ETCD_LOG_DEBUG(tag, fmt, ...)
ETCD_LOG_INFO(tag, fmt, ...)
ETCD_LOG_WARN(tag, fmt, ...)
ETCD_LOG_ERROR(tag, fmt, ...)
```

使用边界：

- 用户负责实现 `galay::kernel::BaseLogger` 并通过 `galay::etcd::log::set()` 注入。
- 日志标签统一带有 `[etcd]` 前缀，内部会继续追加 `[sync]`、`[async]`、`[watch]` 等子标签。
- `galay.etcd` module 已导出 `etcd_log.h`；header 模式也可以直接 `#include <galay/cpp/galay-etcd/base/etcd_log.h>`。

## 3. 同步客户端

`galay-etcd/sync/etcd_client.h` 中的结果类型：

```cpp
using EtcdVoidResult = std::expected<void, EtcdError>;
```

### `EtcdClientBuilder`

```cpp
class EtcdClientBuilder {
public:
    EtcdClientBuilder& endpoint(std::string endpoint);
    EtcdClientBuilder& apiPrefix(std::string prefix);
    EtcdClientBuilder& productionConfig(EtcdProductionConfig config);
    EtcdClientBuilder& requestTimeout(std::chrono::milliseconds timeout);
    EtcdClientBuilder& bufferSize(size_t size);
    EtcdClientBuilder& keepAlive(bool enabled);
    EtcdClientBuilder& config(EtcdConfig config);
    EtcdClient build() const;
    EtcdConfig& buildConfig();
    const EtcdConfig& buildConfig() const;
};
```

### `EtcdClient`

```cpp
class EtcdClient {
public:
    using PipelineOpType = galay::etcd::PipelineOpType;
    using PipelineOp = galay::etcd::PipelineOp;
    using PipelineItemResult = galay::etcd::PipelineItemResult;

    explicit EtcdClient(EtcdConfig config = {});
    ~EtcdClient();

    EtcdClient(const EtcdClient&) = delete;
    EtcdClient& operator=(const EtcdClient&) = delete;
    EtcdClient(EtcdClient&&) = delete;
    EtcdClient& operator=(EtcdClient&&) = delete;

    EtcdBoolResult connect();
    EtcdBoolResult close();

    EtcdBoolResult put(const std::string& key,
                       const std::string& value,
                       std::optional<int64_t> lease_id = std::nullopt);

    EtcdGetResult get(const std::string& key,
                      bool prefix = false,
                      std::optional<int64_t> limit = std::nullopt);

    EtcdDeleteResult del(const std::string& key, bool prefix = false);
    EtcdLeaseGrantResult grantLease(int64_t ttl_seconds);
    EtcdLeaseGrantResult keepAliveOnce(int64_t lease_id);
    EtcdPipelineResult pipeline(std::span<const PipelineOp> operations);
    EtcdPipelineResult pipeline(std::vector<PipelineOp> operations);
    EtcdClientStats getStats() const;

    bool connected() const;
};
```

生命周期与所有权：

- `EtcdClient` 是**非拷贝、非移动**的状态型对象；需要把它放在最终使用位置上，而不是指望后续按值搬运
- `EtcdClientBuilder::build()` 通过直接构造返回一个 prvalue；常见用法是立刻绑定到局部变量或类成员
- 析构函数会在 socket 仍然打开时执行关闭；生产代码里仍建议显式调用 `close()`，让关闭点更可控

语义补充：

- `connect()` 会校验 endpoint，并在配置了 `request_timeout` 时把它应用到同步建连与后续 socket 收发超时
- endpoint 解析接受 `https://...` 语法，但当前 `EtcdClient` 在构造期会把 secure endpoint 记为错误；`connect()` 时返回 `InvalidEndpoint`
- `connect()` 在“已经连接”时直接成功返回，不重复建连
- `close()` 在“已经关闭”时也会直接成功返回
- `get(..., true, limit)` 与 `del(..., true)` 使用 etcd 前缀 range 语义
- `keepAliveOnce()` 在未开启 `request_timeout` 时，会对这次续约请求使用固定 5 秒超时
- `pipeline()` 是固定格式的 txn 批量请求：`compare=[]`、`failure=[]`，只公开 success 分支

## 4. 异步客户端

`galay-etcd/base/etcd_types.h` 中公开了这些结果类型：

```cpp
using EtcdVoidResult = std::expected<void, EtcdError>;
using EtcdBoolResult = std::expected<bool, EtcdError>;
using EtcdGetResult = std::expected<std::vector<EtcdKeyValue>, EtcdError>;
using EtcdDeleteResult = std::expected<int64_t, EtcdError>;
using EtcdLeaseGrantResult = std::expected<int64_t, EtcdError>;
using EtcdPipelineResult = std::expected<std::vector<PipelineItemResult>, EtcdError>;
```

结果别名补充说明：

- `EtcdBoolResult` 用于 `connect/close/put` 这类“成功但无复杂 payload”的操作
- `EtcdGetResult`、`EtcdDeleteResult`、`EtcdLeaseGrantResult`、`EtcdPipelineResult` 描述的是结构化 payload 的形状
- 同步与异步路径都直接返回这些结果值，不再依赖 `last*()` 最近结果访问器

### `AsyncEtcdClientBuilder`

```cpp
class AsyncEtcdClientBuilder {
public:
    AsyncEtcdClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler);
    AsyncEtcdClientBuilder& endpoint(std::string endpoint);
    AsyncEtcdClientBuilder& apiPrefix(std::string prefix);
    AsyncEtcdClientBuilder& productionConfig(EtcdProductionConfig config);
    AsyncEtcdClientBuilder& requestTimeout(std::chrono::milliseconds timeout);
    AsyncEtcdClientBuilder& bufferSize(size_t size);
    AsyncEtcdClientBuilder& keepAlive(bool enabled);
    AsyncEtcdClientBuilder& config(EtcdConfig config);
    AsyncEtcdClient build() const;
    EtcdConfig& buildConfig();
    const EtcdConfig& buildConfig() const;
};
```

### `AsyncEtcdClusterAttempt`

```cpp
struct AsyncEtcdClusterAttempt {
    size_t endpoint_index = 0;
    size_t attempt = 0;
    EtcdConfig config{};
    std::chrono::milliseconds backoff = std::chrono::milliseconds::zero();
};
```

语义补充：

- `config` 是已经注入 endpoint / `api_prefix` / timeout 等 builder 配置的副本
- 它只描述“下一次应该怎么尝试”，不代表库已经替调用侧发起了网络请求

### `AsyncEtcdClusterClientBuilder`

```cpp
class AsyncEtcdClusterClientBuilder {
public:
    AsyncEtcdClusterClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler);
    AsyncEtcdClusterClientBuilder& endpoint(std::string endpoint);
    AsyncEtcdClusterClientBuilder& apiPrefix(std::string prefix);
    AsyncEtcdClusterClientBuilder& productionConfig(EtcdProductionConfig config);
    AsyncEtcdClusterClientBuilder& requestTimeout(std::chrono::milliseconds timeout);
    AsyncEtcdClusterClientBuilder& bufferSize(size_t size);
    AsyncEtcdClusterClientBuilder& keepAlive(bool enabled);
    AsyncEtcdClusterClientBuilder& config(EtcdConfig config);
    AsyncEtcdClusterClient build() const;
    EtcdConfig& buildConfig();
    const EtcdConfig& buildConfig() const;
};
```

语义补充：

- 该 builder 复用与 `AsyncEtcdClientBuilder` 接近的配置 surface
- `build()` 返回的是 offline policy wrapper，不是具备 `put/get/delete` 的 async cluster KV client

### `AsyncEtcdClusterClient`

```cpp
class AsyncEtcdClusterClient {
public:
    using Attempt = AsyncEtcdClusterAttempt;
    using AttemptResult = std::expected<Attempt, EtcdError>;
    using AttemptAwaitable = galay::kernel::ReadyAwaitable<AttemptResult>;

    explicit AsyncEtcdClusterClient(galay::kernel::IOScheduler* scheduler = nullptr,
                                    EtcdConfig config = {});

    AttemptAwaitable beginAttempt();
    AttemptAwaitable nextAttempt(const Attempt& previous, EtcdError error);
    void markSuccess(const Attempt& attempt,
                     std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

    const std::vector<EtcdEndpointHealthSnapshot>& getEndpointSnapshots() const;
    EtcdClientStats getStats() const;
    galay::kernel::IOScheduler* scheduler() const;
};
```

语义补充：

- `beginAttempt()` / `nextAttempt()` 都是立即就绪的 awaitable，用来承载 coroutine-friendly 的离线重试决策
- 调用侧需要拿着 `Attempt.config` 自己决定如何执行真实 I/O；库当前没有 async cluster `connect/put/get/del`
- `markSuccess()` 只更新 snapshot / stats，并为 `StickyLeader` 留下最近一次成功端点 hint
- `StickyLeader` 仍然不是 leader status 感知；这里只是重试策略表面

### `AsyncEtcdClient`

```cpp
class AsyncEtcdClient {
public:
    template <typename AwaitableType>
    class IoAwaitableBase;

    class ConnectAwaitable;
    class CloseAwaitable;
    class PostJsonAwaitable;
    class JsonOpAwaitableBase;
    class PutAwaitable;
    class GetAwaitable;
    class DeleteAwaitable;
    class GrantLeaseAwaitable;
    class KeepAliveAwaitable;
    class PipelineAwaitable;

    using PipelineOpType = galay::etcd::PipelineOpType;
    using PipelineOp = galay::etcd::PipelineOp;
    using PipelineItemResult = galay::etcd::PipelineItemResult;

    AsyncEtcdClient(galay::kernel::IOScheduler* scheduler,
                    EtcdConfig config = {});

    AsyncEtcdClient(const AsyncEtcdClient&) = delete;
    AsyncEtcdClient& operator=(const AsyncEtcdClient&) = delete;
    AsyncEtcdClient(AsyncEtcdClient&&) = delete;
    AsyncEtcdClient& operator=(AsyncEtcdClient&&) = delete;

    ConnectAwaitable connect();
    CloseAwaitable close();

    PutAwaitable put(const std::string& key,
                     const std::string& value,
                     std::optional<int64_t> lease_id = std::nullopt);

    GetAwaitable get(const std::string& key,
                     bool prefix = false,
                     std::optional<int64_t> limit = std::nullopt);

    DeleteAwaitable del(const std::string& key, bool prefix = false);
    GrantLeaseAwaitable grantLease(int64_t ttl_seconds);
    KeepAliveAwaitable keepAliveOnce(int64_t lease_id);
    PipelineAwaitable pipeline(std::span<const PipelineOp> operations);
    PipelineAwaitable pipeline(std::vector<PipelineOp> operations);
    EtcdBoolResult watch(const std::string& key, WatchTaskHandler handler);
    EtcdBoolResult watch(const std::string& key, WatchFunctionHandler handler);
    EtcdClientStats getStats() const;

    bool connected() const;
};
```

生命周期与所有权：

- `AsyncEtcdClient` 也是**非拷贝、非移动**的状态型对象
- 各个公开 awaitable 内部都持有 `AsyncEtcdClient*`；因此 client 实例必须活到对应 `co_await` 完成之后
- `AsyncEtcdClientBuilder::build()` 同样返回 prvalue，建议直接绑定到最终变量 / 成员

公开 nested base 的定位：

- `IoAwaitableBase<AwaitableType>` 与 `JsonOpAwaitableBase` 只服务于 awaitable 复用
- 它们不是常规调用入口，调用侧应通过 `connect()/put()/get()/.../watch()` 进入

### `PostJsonAwaitable`

```cpp
class PostJsonAwaitable {
public:
    PostJsonAwaitable(AsyncEtcdClient& client,
                      std::string api_path,
                      std::string body,
                      std::optional<std::chrono::milliseconds> force_timeout);

    PostJsonAwaitable(const PostJsonAwaitable&) = delete;
    PostJsonAwaitable& operator=(const PostJsonAwaitable&) = delete;
    PostJsonAwaitable(PostJsonAwaitable&&) noexcept = default;
    PostJsonAwaitable& operator=(PostJsonAwaitable&&) noexcept = default;
    ~PostJsonAwaitable();

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    EtcdVoidResult await_resume();
};
```

语义补充：

- 这是一个**公开但偏底层**的 HTTP JSON POST awaitable；`AsyncEtcdClient` 的具体业务 awaitable 都通过它发起请求
- `api_path` 会与 client 当前的 `api_prefix` 拼接，形成最终请求路径
- 如果 client 尚未连接，构造函数会生成 `NotConnected` 错误并让 awaitable 立即就绪
- `force_timeout` 有值时优先使用它；否则在启用了 `request_timeout` 时沿用配置超时
- `await_suspend()` 直接转发到底层 `HttpSessionAwaitable`
- `await_resume()` 会映射 HTTP / kernel 错误到 `EtcdError`，并在成功时直接返回响应体解析后的结构化结果
- HTTP 状态码不在 `2xx` 区间时，`await_resume()` 直接返回 `EtcdErrorType::Server`

### 公开 awaitable 的 `await_*` 语义

#### `ConnectAwaitable`

- 来源：`client.connect()`
- `await_ready()`：当 `IOScheduler*` 为空、endpoint 无效、或 client 已经连上并持有 socket / session 时立即返回 `true`
- `await_suspend()`：把协程挂到 `TcpSocket::connect(...)`
- `await_resume()`：
  - 若前面没有真正启动 I/O，则直接回放当前 `EtcdBoolResult`
  - I/O 成功后创建 `HttpSession`、置 `connected() = true`
  - I/O 失败或建 `HttpSession` 失败时，返回映射后的 `EtcdError`

#### `CloseAwaitable`

- 来源：`client.close()`
- `await_ready()`：当 client 当前没有 socket 时立即返回 `true`
- `await_suspend()`：把协程挂到 `TcpSocket::close()`
- `await_resume()`：
  - 如果底层 close 出错，返回映射后的连接错误
  - 无论成功还是失败，都会重置 `HttpSession` / socket，并把 `connected() = false`

#### `PutAwaitable` / `GetAwaitable` / `DeleteAwaitable` / `GrantLeaseAwaitable` / `KeepAliveAwaitable` / `PipelineAwaitable`

它们共享同一套 await 行为框架：

- 构造函数都会先重置当前错误状态，再调用 request builder 生成 JSON body
- 如果参数校验或 request body 生成失败，构造阶段就会生成错误并让 awaitable 立即就绪
- `await_ready()` / `await_suspend()` 统一委托给内部 `PostJsonAwaitable`
- `await_resume()` 都会先完成 HTTP POST，再做各自的响应解析并直接返回结果值

各个具体 awaitable 的差异如下：

- `PutAwaitable`
  - 路径：`/kv/put`
  - 解析：`parsePutResponse(...)`
  - 成功返回：`EtcdBoolResult{true}`
- `GetAwaitable`
  - 路径：`/kv/range`
  - 解析：`parseGetResponseKvs(...)`
  - 成功返回：`EtcdGetResult`
- `DeleteAwaitable`
  - 路径：`/kv/deleterange`
  - 解析：`parseDeleteResponseDeletedCount(...)`
  - 成功返回：`EtcdDeleteResult`
- `GrantLeaseAwaitable`
  - 路径：`/lease/grant`
  - 解析：`parseLeaseGrantResponseId(...)`
  - 成功返回：`EtcdLeaseGrantResult`
- `KeepAliveAwaitable`
  - 路径：`/lease/keepalive`
  - 解析：`parseLeaseKeepAliveResponseId(..., expected_lease_id)`
  - 成功返回：`EtcdLeaseGrantResult`
  - 超时补充：当 `request_timeout` 没有启用时，这个 awaitable 会对本次请求强制使用 5 秒超时
- `PipelineAwaitable`
  - 路径：`/kv/txn`
  - 解析：`parsePipelineTxnResponse(...)`
  - 成功返回：`EtcdPipelineResult`
  - 额外状态：构造时会捕获一份 `PipelineOpType` 列表，用于在响应阶段按操作顺序解释每一项 txn 返回

## 5. `galay::etcd::internal` source-tree helper surface

`src/cpp/galay-etcd/base/etcd_internal.h` 是源码树内部 helper 集合（`galay::etcd::internal`），用于库实现与仓内测试。

使用边界：

- 当前它**不在** `galay.etcd` module 的导出边界里
- 当前它**不在**安装/export 契约里
- 所有函数都是 inline / header-only 形式；当前建议仅在源码树内部使用
- 同一头里还有一些更底层的数字 / 字符串 / base64 / simdjson 辅助函数；下面优先列出最适合作为外部调用入口的分组 API

### endpoint / prefix 相关 helper

```cpp
std::string normalizeApiPrefix(std::string prefix);

struct ParsedEndpoint {
    std::string host;
    uint16_t port = 0;
    bool secure = false;
    bool ipv6 = false;
};

std::expected<ParsedEndpoint, std::string> parseEndpoint(const std::string& endpoint);
std::string buildHostHeader(const std::string& host, uint16_t port, bool ipv6);
```

语义补充：

- `normalizeApiPrefix()` 会补齐前导 `/`，并移除尾部多余 `/`；空字符串会规范成 `/v3`
- `parseEndpoint()` 负责从 `http://...` / `https://...` endpoint 中解析 host、port、scheme 与 IPv6 标记，并在未显式给出端口时自动补 `80` / `443`
- `parseEndpoint()` 可以把 `https://...` 解析成 `secure = true`；但这不等于客户端真的支持 TLS，当前同步 / 异步客户端随后都会拒绝 secure endpoint
- `buildHostHeader()` 会按 IPv4 / IPv6 形式生成 HTTP `Host` 头值

### request body builder

```cpp
std::expected<std::string, EtcdError> buildPutRequestBody(
    std::string_view key,
    std::string_view value,
    std::optional<int64_t> lease_id = std::nullopt);

std::expected<std::string, EtcdError> buildGetRequestBody(
    std::string_view key,
    bool prefix = false,
    std::optional<int64_t> limit = std::nullopt);

std::expected<std::string, EtcdError> buildDeleteRequestBody(
    std::string_view key,
    bool prefix = false);

std::expected<std::string, EtcdError> buildLeaseGrantRequestBody(int64_t ttl_seconds);
std::expected<std::string, EtcdError> buildLeaseKeepAliveRequestBody(int64_t lease_id);
std::expected<std::string, EtcdError> buildTxnBody(std::span<const PipelineOp> operations);
std::expected<std::string, EtcdError> buildTxnBody(const std::vector<PipelineOp>& operations);
```

语义补充：

- 这些 builder 都返回**最终 JSON body 字符串**，不负责拼完整 HTTP request
- `buildPutRequestBody()` 要求 `key` 非空；`lease_id` 提供时必须为正数
- `buildGetRequestBody()` 要求 `key` 非空；`limit` 提供时必须为正数；`prefix=true` 时会自动生成 etcd range end
- `buildDeleteRequestBody()` 要求 `key` 非空；`prefix=true` 时同样生成 range end
- `buildLeaseGrantRequestBody()` 要求 `ttl_seconds > 0`
- `buildLeaseKeepAliveRequestBody()` 要求 `lease_id > 0`
- `buildTxnBody()` 要求操作列表非空，且每个 `PipelineOp` 的 `key` 非空；如果给了 `limit` / `lease_id`，也都必须为正数
- `buildTxnBody()` 当前固定生成 `{"compare":[],"success":[...],"failure":[]}` 结构；这正是当前 pipeline API 没有公开 compare / failure DSL 的原因

### response parser

```cpp
std::expected<simdjson::dom::object, EtcdError> parseEtcdSuccessObject(
    const std::string& body,
    const std::string& context);

std::expected<std::vector<EtcdKeyValue>, EtcdError> parseKvsFromObject(
    const simdjson::dom::object& object,
    const std::string& context);

std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineResponses(
    const simdjson::dom::object& root,
    std::span<const PipelineOpType> operation_types);

std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineResponses(
    const simdjson::dom::object& root,
    std::span<const PipelineOp> operations);

std::expected<void, EtcdError> parsePutResponse(const std::string& body);
std::expected<std::vector<EtcdKeyValue>, EtcdError> parseGetResponseKvs(const std::string& body);
std::expected<int64_t, EtcdError> parseDeleteResponseDeletedCount(const std::string& body);
std::expected<int64_t, EtcdError> parseLeaseGrantResponseId(const std::string& body);
std::expected<int64_t, EtcdError> parseLeaseKeepAliveResponseId(
    const std::string& body,
    int64_t expected_lease_id);

std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineTxnResponse(
    const std::string& body,
    std::span<const PipelineOpType> operation_types);

std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineTxnResponse(
    const std::string& body,
    std::span<const PipelineOp> operations);
```

语义补充：

- `parseEtcdSuccessObject()` 是通用入口：把 body 解析为 JSON object，并把 simdjson 错误映射成 `EtcdErrorType::Parse`
- `parseKvsFromObject()` 从 etcd 返回对象中解码 `kvs` 数组，生成 `std::vector<EtcdKeyValue>`
- `parsePutResponse()` 当前只在 body 看起来包含 etcd error 字段时进一步做对象解析；普通成功 body 会直接视为成功
- `parseGetResponseKvs()` / `parseDeleteResponseDeletedCount()` / `parseLeaseGrantResponseId()` / `parseLeaseKeepAliveResponseId()` 分别对应公开的 `get` / `del` / `grantLease` / `keepAliveOnce` 解析语义
- `parseLeaseGrantResponseId()` 要求响应里存在 `ID` 字段；缺失时返回 `Parse` 错误
- `parseLeaseKeepAliveResponseId()` 如果响应里带了 `ID` 且它与期望租约 ID 不一致，会返回 `Parse` 错误；否则返回期望租约 ID
- `parsePipelineResponses()` / `parsePipelineTxnResponse()` 会检查：
  - `succeeded` 字段若显式为 `false`，则按 `Server` 错误返回
  - `responses` 数组是否存在，且长度是否与操作数一致
  - 每一项是否含有与操作类型匹配的 `response_put` / `response_range` / `response_delete_range`

## 6. 结果值的使用规则

当前公开 API 不再暴露 `last*()` 最近结果访问器。

统一规则是：

- `connect()` / `close()` / `put()` 成功后返回 `EtcdBoolResult{true}`
- `get()` / `pipeline()` 成功后直接从 `value()` 读取结构化结果
- `grantLease()` / `keepAliveOnce()` 成功后直接从 `value()` 读取租约 ID
- `del()` 成功后直接从 `value()` 读取删除数

## 7. 调用顺序、返回与失败语义

两条客户端路径都遵循同一套业务语义：

- 在第一次 `put/get/del/grantLease/keepAliveOnce/pipeline` 之前先完成 `connect()`
- 同步路径直接返回结构化 `std::expected<value, EtcdError>`
- 异步路径的公开 awaitable 在 `co_await` 后返回同语义的结构化结果
- 调用侧应直接消费返回值，而不是再回头读 client 状态缓存

失败语义：

- 参数、endpoint、网络、HTTP、解析、服务端错误都统一落到 `EtcdError`
- `keepalive` 只控制传输层连接保持，不自动替代 etcd 租约续约
- 异步请求类 awaitable 如果在构造阶段就发现参数错误 / 未连接 / endpoint 无效，可能根本不会挂起协程，而是直接在 `await_ready()` / `await_resume()` 路径返回错误

生命周期与共享边界：

- `EtcdClient` / `AsyncEtcdClient` 都是状态型对象，但公开结果已改为“直接返回值”
- 仍不建议多个线程 / 协程 / 调用方共享同一个客户端实例
- `AsyncEtcdClient` 不能早于其未完成的 awaitable 销毁，因为这些 awaitable 内部保存的是原始 client 指针

## 8. 交叉验证入口

- 同步基础示例：`examples/etcd/include/e1_basic.cc`
- 异步基础示例：`examples/etcd/include/e2_basic.cc`
- 测试入口统一位于 `test/`，用于交叉验证同步、异步、prefix 与 pipeline 语义
- 同步 smoke：`test/etcd/t1_smoke.cc`
- prefix / range 语义：`test/etcd/t2_prefix.cc`
- pipeline 语义：`test/etcd/t3_pipe.cc`、`test/etcd/t5_pipe.cc`
- 异步 smoke：`test/etcd/t4_smoke.cc`
- 内部 helper / parser 交叉验证：`test/etcd/t6_helpers.cc`
