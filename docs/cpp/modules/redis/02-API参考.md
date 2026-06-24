# 02-API参考

本页按 `galay-redis/CMakeLists.txt` 的安装规则与 `galay-redis/module/galay_redis.cppm` 的模块接口整理当前公开 API。这里只记录真实安装/导出的头文件、目标名和返回类型，不混入 benchmark 结论或 FAQ。

## 目标与导出入口

| 项目 | 真实名字 | 说明 |
|---|---|---|
| 源码树目标 | `galay-redis` | 当前仓库真实构建目标 |
| 安装后导入目标 | `galay-redis::galay-redis` | `find_package(galay-redis CONFIG REQUIRED)` 后可直接链接 |
| 可选模块目标 | `galay-redis-modules` / `galay-redis::galay-redis-modules` | 只有满足模块工具链条件时才生成/导出 |
| C++23 模块名 | `galay.redis` | 模块接口文件：`galay-redis/module/galay_redis.cppm` |
| 安装头目录 | `include/galay/cpp/galay-redis` | `galay-redis/CMakeLists.txt` 会安装公开 `.h` / `.hpp` / `.inl`，并排除 `sync/` 遗留目录 |
| 条件安装模块目录 | `include/galay/cpp/galay-redis/module` | `GALAY_REDIS_INSTALL_MODULE_INTERFACE=ON` 时安装 `.cppm` 接口文件 |

与公开表面直接相关的事实：

- `install(DIRECTORY ...)` 会安装当前仓库里的公开 `.h` / `.hpp` / `.inl`，但会显式排除 `sync/` 遗留目录
- `galay.redis` 显式导出了 `redis_base.h`、`redis_config.h`、`redis_error.h`、`redis_value.h`、`redis_protocol.h`、`connection.h`、`redis_client.h`、`conn_pool.h`、`topology_client.h`
- `protocol/builder.h`、`base/redis_log.h` 会经由 `redis_client.h` 被模块用户间接看到
- `module/module_prelude.hpp` 会随头文件一起安装，因为 `module/galay_redis.cppm` 在 global module fragment 中直接 `#include` 它
- `sync/RedisSession.*` 仍存在于源码树中供遗留同步路径与回归测试使用，但不再进入安装/模块公开合同
- `module/galay_redis.cppm` 在模块编译开启时经 `FILE_SET CXX_MODULES` 安装；模块编译关闭但 `GALAY_REDIS_INSTALL_MODULE_INTERFACE=ON` 时，仍会直接安装源码里的 `.cppm`

## 头文件与模块可见性总览

| 头文件 | 主要公开类型 | 模块可见性 | 说明 |
|---|---|---|---|
| `galay-redis/base/redis_base.h` | `KVPair`、`KeyType`、`ValType`、`ScoreValType` | 显式导出 | 基础 concepts |
| `galay-redis/base/redis_config.h` | `RedisSessionConfig`、`AsyncRedisConfig` | 显式导出 | 同步会话配置与 async 超时/缓冲配置 |
| `galay-redis/base/redis_error.h` | `RedisErrorType`、`RedisErrorCode`、`RedisError` | 显式导出 | 统一错误对象 |
| `galay-redis/base/redis_log.h` | `galay::redis::log::set/get`、`REDIS_LOG_*` | 间接可见 | 由 `redis_client.h` 引入 |
| `galay-redis/base/redis_value.h` | `RedisValue`、`RedisAsyncValue` | 显式导出 | async / sync 共用值包装 |
| `galay-redis/protoc/redis_protocol.h` | `RespType`、`RespData`、`RedisReply`、`RespParser`、`RespEncoder` | 显式导出 | RESP2 / RESP3 解析与编码 |
| `galay-redis/protoc/connection.h` | `protocol::Connection` | 显式导出 | 同步 TCP 封装 |
| `galay-redis/protoc/builder.h` | `RedisCommandView`、`RedisEncodedCommand`、`RedisCommandBuilder` | 间接可见 | 命令构建与 batch view |
| `galay-redis/async/redis_client.h` | `RedisConnectOptions`、`RedisClientBuilder`、`RedisClient` | 显式导出 | 单连接 async 主入口 |
| `galay-redis/async/conn_pool.h` | `ConnectionPoolConfig`、`PooledConnection`、`RedisConnectionPool`、`ScopedConnection` | 显式导出 | 连接池 |
| `galay-redis/async/topology_client.h` | `RedisNodeAddress`、`RedisClusterNodeAddress`、`RedisMasterSlaveClientBuilder`、`RedisMasterSlaveClient`、`RedisClusterClientBuilder`、`RedisClusterClient` | 显式导出 | 主从 / Sentinel / Cluster |
| `galay-redis/sync/redis_session.h` | `RedisSession` | 源码树遗留，仅本地回归使用 | 不安装、不导出到模块 |

## 已安装但不导出新类型的文件

- `galay-redis/module/module_prelude.hpp`：模块构建用的预包含头，本身不声明业务 API，但会作为安装产物出现在 `include/galay/cpp/galay-redis/module`

## `redis_base.h`

头文件：`galay-redis/base/redis_base.h`

公开 concepts：

- `KVPair = std::pair<std::string, std::string>`
- `KeyType = std::string`
- `ValType = std::string | int64_t | double`
- `ScoreValType = std::pair<double, std::string>`

这些 concepts 主要作为同步命令与基础类型约束使用。

## `RedisSessionConfig`

头文件：`galay-redis/base/redis_config.h`

公开字段：

- `host`
- `port`
- `username`
- `password`
- `db_index`
- `version`
- `connect_timeout_ms`
- `bind_address`
- `reuse_address`
- `unix_socket_path`

语义要点：

- 这套配置服务于源码树中的同步 `RedisSession`
- 配置以显式字段表达，不再使用类型擦除的连接选项包
- `RedisSession::connect()` 会使用构造时保存的 `host`、`port`、`username`、`password`、`db_index`、`version` 和 `connect_timeout_ms`
- 当前底层同步连接只支持 host / port / timeout
- `bind_address`、`reuse_address`、`unix_socket_path` 是预留字段，当前不会启用绑定本地地址或 Unix 域套接字传输

## `RedisError`

头文件：`galay-redis/base/redis_error.h`

公开错误类型：

- `enum RedisErrorType`
- `using RedisErrorCode = RedisErrorType`
- `constexpr RedisErrorType NetworkError`
- `constexpr RedisErrorType ConnectionClosed`
- `class RedisError`

`RedisErrorType` 覆盖的类别包括：

- URL / host / port / db index / address type / version 校验错误
- 连接、认证、命令、超时、发送、接收、解析、网络与内部错误
- 连接关闭与缓冲区溢出错误

`RedisError` 真实公开接口：

- `RedisError(RedisErrorType type)`
- `RedisError(RedisErrorType type, std::string extra_msg)`
- `RedisErrorType type() const`
- `std::string message() const`

当前 async 与 sync 路径的 `std::expected<..., RedisError>` 都以这里为统一错误对象。

## Redis 日志入口

头文件：`galay-redis/base/redis_log.h`

公开入口：

- `galay::redis::log::set(galay::kernel::BaseLogger::uptr logger)`
- `galay::redis::log::get() -> galay::kernel::BaseLogger*`
- 日志宏：`REDIS_LOG_ENABLED`、`REDIS_LOG_TRACE`、`REDIS_LOG_DEBUG`、`REDIS_LOG_INFO`、`REDIS_LOG_WARN`、`REDIS_LOG_ERROR`、`REDIS_LOG_CRITICAL`

语义要点：

- `set()` 只影响 `REDIS_LOG_*` 宏产生的 galay-redis 日志，不会启用 kernel、ssl、http 或其他 galay 库日志
- 传入 `nullptr` 等价于禁用 galay-redis 日志
- logger 的所有权通过 `std::unique_ptr<BaseLogger>` 转移给库级槽位，`get()` 返回的裸指针不得由调用方释放
- 未设置 logger 或日志级别被 `minLevel()` 过滤时，日志宏不会执行 `std::format`，也不会求值格式化参数
- 推荐在创建 `RedisClient`、`RedissClient`、`RedisConnectionPool` 或 `RedissConnectionPool` 之前的单线程初始化阶段调用 `set()`

## `AsyncRedisConfig`

头文件：`galay-redis/base/redis_config.h`

关键字段：

- `send_timeout`
- `recv_timeout`
- `buffer_size`

静态辅助函数：

- `withTimeout(send, recv)`
- `withRecvTimeout(recv)`
- `withSendTimeout(send)`
- `noTimeout()`

语义要点：

- async 超时使用 `std::chrono::milliseconds`
- 小于 `0ms` 表示禁用对应超时
- `buffer_size` 默认值是 `65536`
- `buffer_size` 用于创建客户端内部 ring buffer；当前没有公开的自定义 buffer provider API
- examples/tests 常用的是 `noTimeout()`，或者在 awaitable 上叠加 `.timeout(...)`

## `RedisCommandBuilder`

头文件：`galay-redis/protoc/builder.h`

### 核心类型

- `RedisCommandView`
- `RedisEncodedCommand`
- `RedisCommandBuilder`

### 核心方法

| 方法 | 用途 | 覆盖来源 |
|---|---|---|
| `clear()` | 清空 builder 以便复用 | `test/t9_span.cc` |
| `reserve(command_count, arg_count, storage_bytes)` | 预留 batch 空间 | `examples/include/e2_pipeline.cc`、`test/t8_batch.cc` |
| `append(cmd, args...)` | 追加 batch 命令 | `examples/include/e2_pipeline.cc` |
| `commands()` | 取 `std::span<const RedisCommandView>` | `test/t9_span.cc` |
| `build()` / `release()` | 生成单个 `RedisEncodedCommand` | `test/t10_command.cc` |
| `command(cmd, args..., expected_replies)` | 构造任意 raw command | `test/t10_command.cc` |

### 便捷命令封装

`builder.h` 还公开了当前已对齐的命令族：

- 连接命令：`auth`、`select`、`ping`、`echo`
- Pub/Sub：`publish`、`subscribe`、`unsubscribe`、`psubscribe`、`punsubscribe`
- 拓扑命令：`role`、`replicaof`、`readonly`、`readwrite`、`clusterInfo`、`clusterNodes`、`clusterSlots`
- String：`get`、`set`、`setex`、`del`、`exists`、`incr`、`decr`
- Hash：`hget`、`hset`、`hdel`、`hgetAll`
- List：`lpush`、`rpush`、`lpop`、`rpop`、`llen`、`lrange`
- Set：`sadd`、`srem`、`smembers`、`scard`
- Sorted Set：`zadd`、`zrem`、`zrange`、`zscore`

## `RedisClientBuilder` 与 `RedisClient`

头文件：`galay-redis/async/redis_client.h`

### 类型别名与配置结构

- `class RedisBorrowedCommand`
- `using RedisExchangeOperation = detail::RedisExchangeOperation`
- `using RedisConnectOperation = detail::RedisConnectOperation`
- `using RedisResult = std::expected<std::vector<RedisValue>, RedisError>`
- `using RedisVoidResult = std::expected<void, RedisError>`
- `struct RedisConnectOptions`

`RedisConnectOptions` 字段：

- `username`
- `password`
- `db_index`
- `version`

注意：

- 当前 async 示例和测试都使用默认 `version = 2`
- 实现里 `version == 6` 会被解释成 IPv6 socket family，其余值走 IPv4；它不是本文档里的 RESP3 切换开关

### `RedisBorrowedCommand`

- 构造：`RedisBorrowedCommand(const std::string& encoded, size_t expected_replies = 1)`
- 访问器：`encoded()`、`expectedReplies()`
- 编译期约束：`std::string&&` 与 `std::string_view` 构造被显式禁用
- 生命周期约束：`RedisBorrowedCommand` 只借用底层 `std::string`；源字符串必须覆盖整个 `co_await client.commandBorrowed(...)`。`batchBorrowed(const std::string&, ...)` 也遵循同样的借用规则
- 角色定位：plain TCP 路径的内部 borrowed fast path 包装，不是替代常规 `RedisCommandBuilder` owning API 的通用表面

### `RedisClientBuilder`

当前公开方法：

- `scheduler(IOScheduler*)`
- `config(AsyncRedisConfig)`
- `sendTimeout(std::chrono::milliseconds)`
- `recvTimeout(std::chrono::milliseconds)`
- `bufferSize(size_t)`
- `buildConfig() const`
- `build()`

`buildConfig()` 会返回 builder 当前累积出的 `AsyncRedisConfig` 快照，不会隐式创建 `RedisClient`。
`bufferSize(size_t)` 会写入 `AsyncRedisConfig::buffer_size`，用于构造客户端内部 ring buffer。

### `RedisClient`

| 方法 | `co_await` 结果 | 说明 |
|---|---|---|
| `connect(url)` | `std::expected<void, RedisError>` | 支持 `redis://user:password@host:port/db_index` |
| `connect(ip, port, options)` | `std::expected<void, RedisError>` | async 主连接入口 |
| `command(RedisEncodedCommand)` | `std::expected<std::optional<std::vector<RedisValue>>, RedisError>` | 单命令发送 |
| `commandBorrowed(const RedisBorrowedCommand&)` | `std::expected<std::optional<std::vector<RedisValue>>, RedisError>` | plain 内部零拷贝快路径；调用方持有的编码字节必须覆盖整个 `co_await` |
| `receive(expected_replies = 1)` | `std::expected<std::optional<std::vector<RedisValue>>, RedisError>` | Pub/Sub 或手动收包 |
| `batch(std::span<const RedisCommandView>)` | `std::expected<std::optional<std::vector<RedisValue>>, RedisError>` | 批量发送 |
| `batchBorrowed(const std::string&, size_t expected_replies)` | `std::expected<std::optional<std::vector<RedisValue>>, RedisError>` | plain 内部预编码 pipeline 快路径；`std::string&&` / `std::string_view` 重载已删除 |
| `close()` | `galay::kernel::CloseAwaitable` | 关闭连接 |
| `isClosed()` | `bool` | 查询连接状态 |

覆盖来源：

- 基础命令：`examples/include/e1_basic.cc`、`test/t1_async.cc`
- timeout：`test/t5_timeout.cc`
- batch：`examples/include/e2_pipeline.cc`、`test/t8_batch.cc`
- raw command：`test/t10_command.cc`
- Pub/Sub 收包：`examples/include/e3_pubsub.cc`
- borrowed fast path surface：`test/t21_fastpath.cc`
- borrowed fast path localhost smoke：`test/t22_fastpath.cc`、`test/t23_pipeline.cc`

### Operation 类型与精确语义

#### `RedisConnectOperation`

- 来源：`RedisClient::connect(url)`、`RedisClient::connect(ip, port, options)`
- 公开别名：`galay::kernel::StateMachineAwaitable<detail::RedisConnectMachine>`
- 构建方式：`AwaitableBuilder<RedisVoidResult>::fromStateMachine(...).build()`
- `await_resume()` 返回 `RedisVoidResult`，即 `std::expected<void, RedisError>`
- 内部状态机会在 `Connect`、`Send`、`Parse`、`Done` 之间推进；若对象已失效，则落到 `Invalid`
- `RedisConnectOptions::version` 当前只有 `6` 会被解释成 IPv6，其余值都按 IPv4 处理；它不是 RESP 版本开关
- `username` 与 `password` 都为空时会跳过 `AUTH`；`db_index == 0` 时会跳过 `SELECT`
- 当前源码里的错误映射比较具体：连接失败返回 `CONNECTION_ERROR`，`AUTH` 发送/接收失败分别映射到 `SEND_ERROR` / `RECV_ERROR`，鉴权失败映射到 `AUTH_ERROR`，解析失败映射到 `PARSE_ERROR`，连接关闭映射到 `CONNECTION_CLOSED`，无可写 iovec 映射到 `BUFFER_OVERFLOW_ERROR`
- 需要注意：`SELECT` 阶段收到 Redis 错误回复时，当前实现映射到 `INVALID_ERROR`；这是源码现状，文档按源码为准
- 若在 `Invalid` 状态下恢复结果，当前实现返回 `INTERNAL_ERROR`

#### `RedisExchangeOperation`

- 来源：`RedisClient::command(...)`、`RedisClient::commandBorrowed(...)`、`RedisClient::receive(...)`、`RedisClient::batch(...)`、`RedisClient::batchBorrowed(...)`
- 公开别名：`galay::kernel::StateMachineAwaitable<detail::RedisExchangeMachine>`
- 构建方式：`AwaitableBuilder<detail::RedisExchangeResult>::fromStateMachine(...).build()`
- `await_resume()` 返回 `std::expected<std::optional<std::vector<RedisValue>>, RedisError>`
- 当前成功完成路径会返回已就绪的 reply `vector<RedisValue>`；当 `expected_replies == 0` 时返回空 `vector`
- `commandBorrowed(...)` / `batchBorrowed(...)` 与 owning 路径共用同一状态机，只是发送阶段直接借用调用方持有的 RESP 编码字节
- borrowed 路径不会接管底层字节所有权，因此传入的 `std::string` 必须在整个 `co_await` 完成前保持存活
- I/O 超时与底层 I/O 错误会先经过 `IOError`，再按 `mapIoErrorToRedisType(...)` 转换成 Redis 侧错误类型
- 连接关闭、RESP 解析失败、缓冲区窗口不足会分别落到 `CONNECTION_CLOSED`、`PARSE_ERROR`、`BUFFER_OVERFLOW_ERROR`
- 若状态机落入 `Invalid`，恢复结果会得到 `INTERNAL_ERROR`

#### 公开 / 内部边界

- 公开头文件里已经不再暴露 `RedisClientAwaitable`、`RedisPipelineAwaitable`、`RedisConnectAwaitable`
- 旧的 `ProtocolSendAwaitable` / `ProtocolRecvAwaitable` 嵌套类型也不再是公共 surface
- 当前公开 awaitable 表面统一收敛为 `RedisConnectOperation` / `RedisExchangeOperation`
- `detail::RedisConnectMachine`、`detail::RedisExchangeMachine` 仍出现在头文件里，是因为 `StateMachineAwaitable` 别名需要这些 machine 类型；它们应按实现细节理解

## `RedisConnectionPool`

头文件：`galay-redis/async/conn_pool.h`

### 配置与值类型

- `struct ConnectionPoolConfig`
- `class PooledConnection`
- `struct RedisConnectionPool::PoolStats`
- `class ScopedConnection`

`ConnectionPoolConfig` 主要字段：

- 地址与鉴权：`host`、`port`、`username`、`password`、`db_index`
- 容量：`min_connections`、`max_connections`、`initial_connections`
- 超时：`acquire_timeout`、`idle_timeout`、`connect_timeout`
- 健康检查：`enable_health_check`、`health_check_interval`
- 重连：`enable_auto_reconnect`、`max_reconnect_attempts`
- 校验：`enable_connection_validation`、`validate_on_acquire`、`validate_on_return`

辅助函数：

- `validate() const`
- `defaultConfig()`
- `create(host, port, min_conn, max_conn)`

### 主类型

| 类型 / 方法 | 说明 | 覆盖来源 |
|---|---|---|
| `RedisConnectionPool(scheduler, config)` | 创建连接池 | `test/t4_pool.cc` |
| `initialize()` | 初始化连接 | `test/t4_pool.cc` |
| `acquire()` | 获取 `shared_ptr<PooledConnection>` | `test/t4_pool.cc` |
| `release(conn)` | 归还连接 | `test/t4_pool.cc` |
| `triggerHealthCheck()` / `triggerIdleCleanup()` | 主动维护 | `conn_pool.h` |
| `warmup()` / `expandPool()` / `shrinkPool()` | 连接池容量控制 | `conn_pool.h` |
| `cleanupUnhealthyConnections()` | 移除不健康连接 | `conn_pool.h` |
| `getStats()` | 统计信息 | `test/t4_pool.cc`、`benchmark/b2_pool.cc` |
| `getConfig()` | 读取当前配置 | `conn_pool.h` |
| `shutdown()` | 同步关闭连接池 | `test/t4_pool.cc` |
| `ScopedConnection` | RAII 封装 | `conn_pool.h` |

### Awaitable 精确语义

#### `PoolInitializeAwaitable`

- 来源：`RedisConnectionPool::initialize()`
- `await_resume()` 返回 `RedisVoidResult`
- 当前实现会通过同步路径 `getConnectionSync()` 尝试创建最多 `initial_connections` 个连接条目
- 如果最终创建数量仍小于 `min_connections`，会返回 `CONNECTION_ERROR`
- 成功路径下会把连接池标记为 initialized，后续 `acquire()` 才能进入正常获取流程
- 这里要避免过度承诺：源码注释与实现都表明它走的是同步创建路径，文档不把它表述成“已完成全部 async 握手并建立可用 Redis 会话”

#### `PoolAcquireAwaitable`

- 来源：`RedisConnectionPool::acquire()`
- `await_resume()` 返回 `std::expected<std::shared_ptr<PooledConnection>, RedisError>`
- 若连接池尚未初始化，会返回 `"Connection pool not initialized"`；若已进入 shutdown，会返回 `"Connection pool is shutting down"`
- 正常路径会优先复用健康的空闲连接；若当前总连接数还没到 `max_connections`，实现也可能尝试同步新建连接
- 在超时窗口内仍拿不到连接时，当前实现返回 `TIMEOUT_ERROR`，消息为 `"No available connections"`

#### 内部边界说明

- `PoolInitializeAwaitable`、`PoolAcquireAwaitable` 是公共 API 的一部分，因为调用者会直接 `co_await` 它们
- `m_result`、内部时间戳与等待状态字段只是 `TimeoutSupport` / 协程编排所需细节，不建议业务层读取或推断更细的生命周期语义

## `topology_client.h`

头文件：`galay-redis/async/topology_client.h`

### 公共值类型

- `RedisCommandResult = std::expected<std::vector<RedisValue>, RedisError>`
- `RedisNodeAddress`
- `RedisClusterNodeAddress`

### Builder 类型

`topology_client.h` 公开了 TCP/TLS 两组 builder，它们都按值累积配置：

- `RedisMasterSlaveClientBuilder`
  - `scheduler(IOScheduler*)`
  - `config(AsyncRedisConfig)`
  - `sendTimeout(std::chrono::milliseconds)`
  - `recvTimeout(std::chrono::milliseconds)`
  - `bufferSize(size_t)`
  - `buildConfig() const`
  - `build()`
- `RedisClusterClientBuilder`
  - `scheduler(IOScheduler*)`
  - `config(AsyncRedisConfig)`
  - `sendTimeout(std::chrono::milliseconds)`
  - `recvTimeout(std::chrono::milliseconds)`
  - `bufferSize(size_t)`
  - `buildConfig() const`
  - `build()`
- `RedissMasterSlaveClientBuilder`
  - 在上面基础上增加 `tlsConfig(RedissClientConfig)`
  - `buildTlsConfig() const`
  - `build()`
- `RedissClusterClientBuilder`
  - 在上面基础上增加 `tlsConfig(RedissClientConfig)`
  - `buildTlsConfig() const`
  - `build()`

### `RedisMasterSlaveClient`

| 方法 | `co_await` 结果 | 说明 |
|---|---|---|
| `connectMaster(address)` | `std::expected<void, RedisError>` | 连接主节点 |
| `addReplica(address)` | `std::expected<void, RedisError>` | 增加副本 |
| `addSentinel(address)` | `std::expected<void, RedisError>` | 增加 Sentinel |
| `setSentinelMasterName(name)` | `void` | 指定 master 名称 |
| `setAutoRetryAttempts(attempts)` | `void` | 设置自动重试次数 |
| `execute(cmd, args, prefer_read, auto_retry)` | `std::expected<std::vector<RedisValue>, RedisError>` | 读写命令 |
| `batch(commands, prefer_read)` | batch awaitable | 批量命令 |
| `refreshFromSentinel()` | `std::expected<std::vector<RedisValue>, RedisError>` | 触发刷新 |
| `master()` / `replica(index)` | 返回底层 `RedisClient` | 用于直接 close 或观测 |

覆盖来源：

- 基本行为：`test/t11_pubsub.cc`
- 并发刷新 single-flight：`test/t12_topology.cc`
- 真实 Sentinel failover：`test/t13_cluster.cc`

### `RedisClusterClient`

| 方法 | `co_await` 结果 | 说明 |
|---|---|---|
| `addNode(node)` | `std::expected<void, RedisError>` | 增加 seed 节点 |
| `setSlotRange(index, start, end)` | `void` | 手动设置槽区间 |
| `setAutoRefreshInterval(interval)` | `void` | 自动刷新节流 |
| `execute(cmd, args, routing_key, auto_retry)` | `std::expected<std::vector<RedisValue>, RedisError>` | 路由执行 |
| `batch(commands, routing_key)` | batch awaitable | 同路由批量发送 |
| `refreshSlots()` | `std::expected<std::vector<RedisValue>, RedisError>` | 拉取 `CLUSTER SLOTS` |
| `keySlot(key)` | `uint16_t` | 计算 key slot |
| `nodeCount()` / `node(index)` | 节点探查 | 用于 close 与调试 |

### Awaitable 与内部边界

#### 拓扑方法现在返回 `Task<RedisCommandResult>`

- 来源：`RedisMasterSlaveClient::execute(...)`、`RedisMasterSlaveClient::refreshFromSentinel()`、`RedisClusterClient::execute(...)`、`RedisClusterClient::refreshSlots()`
- 返回类型：`galay::kernel::Task<RedisCommandResult>`
- `RedisCommandResult` 仍然是 `std::expected<std::vector<RedisValue>, RedisError>`
- 调用方式直接写成 `auto result = co_await cluster.refreshSlots();`
- 公开头文件里已经不再暴露 `RedisCommandResultAwaitable`

### TLS 对应类型

当顶层构建选项 `GALAY_BUILD_SSL=ON` 且 Redis 一起构建时，`redis_client.h` / `conn_pool.h` / `topology_client.h` 会公开以下 TLS facade：

- `RedissClient`
- `RedissClientBuilder`
- `RedissClientConfig`
- `RedissConnectionPool`
- `RedissConnectionPoolConfig`
- `RedissMasterSlaveClient`
- `RedissClusterClient`

最常用的 TLS 入口：

- `RedissClient::connect("rediss://host:6380/0")`
- `RedissClientBuilder::tlsConfig(...)`
- `RedissMasterSlaveClientBuilder::tlsConfig(...)`
- `RedissClusterClientBuilder::tlsConfig(...)`

TLS 单连接路径当前返回的 operation 类型是：

- `detail::RedissConnectOperation`
- `detail::RedissExchangeOperation`

其中：

- 开启 SSL 构建时，`command(...)` / `batch(...)` 走 `galay::ssl::SslStateMachineAwaitable<detail::RedissExchangeMachine>`
- 开启 SSL 构建时，`connect(...)` 走 `galay::kernel::StateMachineAwaitable<detail::RedissConnectMachine>`，内部再切到 `SslOperationDriver` 推进握手
- 未开启 SSL 构建时，这两个类型会立即返回“built without SSL support”的 ready-result awaitable

#### 内部但可见的类型

- `RedisMasterSlaveClient::NodeHandle`
- `RedisClusterClient::RedirectInfo`

这两个类型虽然暴露在公共头文件里，但职责都是拓扑客户端内部的路由 / 重定向状态管理。RAG 命中它们时，应优先把它们解释成“实现辅助类型”，而不是推荐直接面向业务代码使用的核心 API。

覆盖来源：

- key slot / route key：`test/t11_pubsub.cc`
- MOVED / ASK / refresh：`test/t13_cluster.cc`

## `redis_protocol.h` 与 `RedisReply`

头文件：`galay-redis/protoc/redis_protocol.h`

公开协议类型：

- `enum class RespType`
- `using RespData = std::variant<...>`
- `class RedisReply`
- `enum class ParseError`
- `class RespParser`
- `class RespEncoder`

`RedisReply` 当前公开接口：

- 构造与拷贝/移动：默认构造、`RedisReply(RespType, RespData)`、拷贝/移动构造与赋值
- 类型判断：`isSimpleString()`、`isError()`、`isInteger()`、`isBulkString()`、`isArray()`、`isNull()`、`isDouble()`、`isBoolean()`、`isMap()`、`isSet()`、`isPush()`
- 取值：`asString()`、`asInteger()`、`asDouble()`、`asBoolean()`、`asArray()`、`asMap()`
- 元信息：`getType()`、`getData()`

补充说明：

- `RespType` 枚举当前还包含 `BlobError`、`VerbatimString`、`BigNumber`、`Attribute`
- 这些更细的 RESP3 marker 没有各自独立的 `RedisReply::isXxx()` 便捷方法，但 `RespParser` 会保留真实 `RespType`
- 读取 payload 时仍分别使用 `asString()`、`asMap()` 或 `asArray()`；判断细分 marker 时看 `getType()`

`RespParser` 当前公开方法：

- `parse(const char* data, size_t length)`
- `parseFast(const char* data, size_t length, RedisReply* out)`
- `reset()`

`RespEncoder` 当前公开方法：

- `encodeSimpleString(...)`
- `encodeError(...)`
- `encodeInteger(...)`
- `encodeBulkString(...)`
- `encodeNull()`
- `encodeArray(...)`
- `encodeCommand(...)` 的多种重载
- `append(...)` 的多种重载
- `appendCommandFast(...)` 的多种重载

覆盖来源：

- 协议解析示例：`test/t3_protocol.cc`
- 值包装使用：`galay-redis/base/redis_value.h`

## `Connection`

头文件：`galay-redis/protoc/connection.h`

命名空间：`galay::redis::protocol`

真实公开方法：

- `Connection()`
- `~Connection()`
- `connect(const std::string& host, int port, uint32_t timeout_ms = 5000)`
- `disconnect()`
- `isConnected() const`
- `send(const std::string& data)`
- `receiveReply()`
- `execute(const std::string& encoded_command)`

返回类型：

- `connect()` / `send()`：`std::expected<void, RedisError>`
- `receiveReply()` / `execute()`：`std::expected<RedisReply, RedisError>`

这是同步 `RedisSession` 使用的底层 TCP + RESP 封装。

## `RedisValue`

头文件：`galay-redis/base/redis_value.h`

公开类型：

- `class RedisValue`
- `class RedisAsyncValue`

常用判定与取值方法：

- RESP2：`isNull`、`isStatus`、`toStatus`、`isError`、`toError`、`isInteger`、`toInteger`、`isString`、`toString`、`isArray`、`toArray`
- RESP3：`isDouble`、`toDouble`、`isBool`、`toBool`、`isMap`、`toMap`、`isSet`、`toSet`、`isAttr`（attribute `|`，取值仍用 `toMap()`）、`isPush`、`toPush`、`isBigNumber`、`toBigNumber`、`isVerb`、`toVerb`
- RESP3 blob error：底层 `RedisReply::getType()` 会保留 `RespType::BlobError`，`RedisValue` 层继续通过 `isError()` / `toError()` 暴露错误文本
- 底层回复访问：`getReply() const`、`getReply()`
- 错误工厂：`RedisValue::fromError(...)`

生命周期要点：

- `toArray()`、`toMap()`、`toSet()`、`toPush()` 都返回独立副本，不借用原始 `RedisValue` 的内部存储
- `RedisValue` 禁止拷贝、允许移动

覆盖来源：

- async 读取：`examples/include/e1_basic.cc`
- Pub/Sub 消息数组：`examples/include/e3_pubsub.cc`
- 大小与行为验证：`test/t14_size.cc`
- awaitable / pool public surface：`test/t15_surface.cc`

## `RedisSession`（源码树遗留同步 API，不属于安装公开合同）

头文件：`galay-redis/sync/redis_session.h`

这是仓库源码树里仍保留的同步阻塞接口，当前主要用于维护遗留实现和 `test/t2_sync.cc` 这类源码树回归。公开方法包括：

- `connect(...)` / `disconnect()`
- `selectDB()` / `flushDB()` / `switchVersion()`
- `get()` / `set()` / `del()` / `exist()` / `setEx()`
- `hget()` / `hset()` / `hgetAll()`
- `lpush()` / `rpush()` / `lrange()`
- `sadd()` / `smembers()` / `srem()`
- `zadd()` / `zrange()` / `zscore()` / `zrem()`
- `redisCommand(...)`

但要注意：

- 当前安装规则会排除 `sync/`，因此 `find_package(galay-redis)` 消费方不会拿到这组头文件
- 当前 `examples/`、`benchmark/` 没有覆盖这条同步路径
- 当前主 README 和快速开始不再把它当作默认入口
- 模块 `galay.redis` 也不会导出这条同步路径
- 如果你仍需要同步 API，请把它视为源码树内部兼容表面，而不是安装消费方的稳定入口

## 模块接口

真实模块信息：

- 模块名：`galay.redis`
- 文件：`galay-redis/module/galay_redis.cppm`
- 可选目标：`galay-redis-modules`

模块构建是否可用由 `galay-redis/CMakeLists.txt` 动态判定；详细条件见 [06-高级主题](06-高级主题.md)。

模块可见性的边界：

- 显式模块导出：`redis_base.h`、`redis_config.h`、`redis_error.h`、`redis_value.h`、`redis_protocol.h`、`connection.h`、`redis_client.h`、`conn_pool.h`、`topology_client.h`
- 通过 `redis_client.h` 间接可见：`redis_log.h`、`builder.h`
- 不在模块/安装合同里：`sync/redis_session.h`

## 统一返回、生命周期与交叉验证语义

当前 async 主路径的 canonical 使用方式是：

- 用 `RedisCommandBuilder` 构造命令
- 用 `RedisClient::command(...)` / `batch(...)` 发送
- 用 `RedisMasterSlaveClient::execute(...)` / `RedisClusterClient::execute(...)` 走拓扑路径

语义边界：

- 当前 async 主路径本质上是协程友好 API，真实协程调用方式见 `examples/include/e1_basic.cc`
- 失败语义统一回到 `RedisError`、`RedisValue` / `RedisAsyncValue` 与对应返回签名，不应把 RESP2 / RESP3 文本内容当成唯一错误判据
- `RedisSession` 是源码树遗留的同步阻塞表面，不是当前 async 主路径的“简写”，也不是安装消费方的默认入口
- `RedisValue` / `RedisAsyncValue` 是当前统一结果容器；RESP2 / RESP3 的细节都应回到这里确认
- 连接池、拓扑客户端、单连接客户端都是状态型对象；检索调用细节时，应把 builder/config、client、value 三层分开理解

交叉验证入口：

- async 基础命令：`examples/include/e1_basic.cc`
- batch / pipeline：`examples/include/e2_pipeline.cc`
- topology / pubsub：`examples/include/e3_pubsub.cc`
- 测试入口：`test/t1_async.cc`、`test/t2_sync.cc`（源码树遗留同步回归）、`test/t15_surface.cc`
- async / sync 基础测试：`test/t1_async.cc`、`test/t2_sync.cc`
- topology / single-flight：`test/t11_pubsub.cc`、`test/t12_topology.cc`
- awaitable / builder surface：`test/t15_surface.cc`

## 不再作为当前 async API 文档入口的旧别名

以下名字在当前 async 头文件里都不应再作为主文档入口：

- `client.get(...)`
- `client.set(...)`
- `client.pipeline(...)`
- `cluster.executeByKey(...)`
- `cluster.executeByKeyAuto(...)`

当前 async 主入口统一为：

- `RedisCommandBuilder`
- `RedisClient::command(...)`
- `RedisClient::batch(...)`
- `RedisMasterSlaveClient::execute(...)`
- `RedisClusterClient::execute(...)`
