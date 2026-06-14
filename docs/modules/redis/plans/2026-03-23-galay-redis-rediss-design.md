# galay-redis Rediss/TLS 适配设计

## 背景

本轮目标是在 `galay-redis` 中完成两件事：

- 对齐最新 `galay-kernel`
- 参考 `galay-http`，新增真正可用的 `rediss://` / TLS 客户端能力

已有上下文：

- `RedisClientAwaitable`、`RedisPipelineAwaitable`、`RedisConnectAwaitable` 已经朝 `task.h + AwaitableBuilder::fromStateMachine(...)` 收敛
- `PoolInitializeAwaitable`、`PoolAcquireAwaitable` 已改为 builder-backed facade
- `RedisTopologyClient` 仍保留 `RedisCommandResultAwaitable` 这类“协调型 awaitable”
- 模块前导与 CMake 依赖顺序还没有完全对齐 `galay-http`
- 当前仓库还没有真正导入 `galay-ssl::galay-ssl`，也没有 `SslSocket` / `SslAwaitableBuilder` 接入层

## 目标

### 功能目标

- 新增可工作的 `RedissClient`
- 支持 `rediss://user:pass@host:port/db` 连接字符串
- TLS 连接完成后仍支持：
  - AUTH
  - SELECT
  - command
  - receive
  - batch / pipeline
- 连接池与 topology 侧也能复用 TLS 传输能力

### 架构目标

- 不再自实现 Redis I/O awaitable 执行核心
- TCP 路径继续使用最新 `galay-kernel` 的 builder/state-machine 体系
- TLS 路径使用最新 `galay-ssl` 的 `SslAwaitableBuilder::fromStateMachine(...)`
- 删除 `RedisCommandResultAwaitable`
- topology 编排层改为直接返回 `Task<Result>`

## 方案比较

### 方案 A：单一 `RedisClient` 运行时切换 TCP/TLS

做法：

- 保留一个 `RedisClient`
- 在内部按 URL 或配置动态切换 `TcpSocket` / `SslSocket`

优点：

- 表面 API 最少变化

缺点：

- awaitable 内核需要运行时分支或类型擦除
- `command` / `batch` / `connect` 的状态机和 socket 控制器很难保持干净
- 调试成本高

结论：不采用。

### 方案 B：参考 `galay-http` 的双 facade + 传输模板复用

做法：

- 保留 `RedisClient` 表示 TCP
- 新增 `RedissClient` 表示 TLS
- 内部复用 `BasicRedisClient<SocketType>`
- TCP 用 `AwaitableBuilder::fromStateMachine(...)`
- TLS 用 `galay::ssl::SslAwaitableBuilder::fromStateMachine(...)`

优点：

- 与 `galay-http` 的组织方式一致
- 不需要为 TLS 引入额外的类型擦除层
- 便于把连接池 / topology 一并扩展到 TLS

缺点：

- 改动面较大

结论：采用。

### 方案 C：只补最小单节点 TLS 客户端

做法：

- 只新增 `RedissClient`
- 连接池 / topology 保持 TCP-only

优点：

- 可最快做出最小 rediss demo

缺点：

- TLS 能力不完整
- 后续连接池与 topology 接入必然返工

结论：不采用。

## 设计决策

### 1. 对外 API 形状

保留 / 新增以下公开类型：

- `RedisClient`
- `RedisClientBuilder`
- `RedissClient`
- `RedissClientBuilder`
- `RedissClientConfig`

API 规则：

- `RedisClient::connect(url)` 只面向 `redis://...`
- `RedissClient::connect(url)` 面向 `rediss://...`
- `RedissClient` 的 TLS 配置最小集包括：
  - `ca_path`
  - `verify_peer`
  - `verify_depth`
  - `server_name`
- Redis 行为配置继续复用 `AsyncRedisConfig`

### 2. 传输复用模型

内部抽出按 socket 类型复用的实现层：

- `BasicRedisClient<TcpSocket>`
- `BasicRedisClient<galay::ssl::SslSocket>`

公开 facade：

- `RedisClient` 封装 TCP 版本
- `RedissClient` 封装 TLS 版本

共享内容：

- RESP 编码/解析
- ring buffer 管理
- Redis 错误映射
- AUTH / SELECT / pipeline / receive 状态推进语义

按传输分流的内容：

- connect / read / write 的 machine action
- TLS handshake
- SNI 与证书校验配置

### 3. Awaitable 体系收敛

#### TCP

- `command` / `receive` / `batch` / `connect` 继续通过 `AwaitableBuilder::fromStateMachine(...)`

#### TLS

- `command` / `receive` / `batch` / `connect` 改为通过
  `galay::ssl::SslAwaitableBuilder::fromStateMachine(...)`

这样 Redis 协议逻辑仍然由共享状态驱动，但：

- TCP machine 发出 `waitConnect / waitWrite / waitReadv`
- TLS machine 发出 `handshake / send / recv`

#### 关键约束

- `SslSocket` 不支持 `readv/writev`
- TLS 路径读取只使用 ring buffer 的第一段可写窗口 `recv()`
- TLS 路径发送使用连续 buffer `send()`
- 解析与缓冲区 consume/produce 逻辑保持共享，不复制两套协议代码

### 4. 连接流程

#### `RedisClient::connect(redis://...)`

- TCP connect
- 可选 AUTH
- 可选 SELECT

#### `RedissClient::connect(rediss://...)`

- TCP connect
- TLS handshake
- 可选 AUTH
- 可选 SELECT

设计上不把 TLS handshake 暴露成额外公共步骤；`connect()` 完成后即表示该 Redis 传输已可用于后续命令。

### 5. 删除 `RedisCommandResultAwaitable`

`RedisTopologyClient` 当前的 `RedisCommandResultAwaitable` 本质上是对内部调度 + waiter 的包装，不再保留。

替代方案：

- `RedisMasterSlaveClient::execute(...)` 直接返回 `Task<RedisCommandResult>`
- `RedisMasterSlaveClient::refreshFromSentinel()` 直接返回 `Task<RedisCommandResult>`
- `RedisClusterClient::execute(...)` 直接返回 `Task<RedisCommandResult>`
- `RedisClusterClient::refreshSlots()` 直接返回 `Task<RedisCommandResult>`

原因：

- `AwaitableBuilder` 适合 I/O sequence，不适合把 `AsyncWaiter` 这类同步原语硬塞进 machine-backed builder
- topology 这层属于跨多个 Redis 操作的业务编排，更适合直接用 `Task<Result>` 表达
- 这样仓库内不再保留为了“等待一个协调结果”而手写的 awaitable facade

### 6. 连接池与 topology 的 TLS 贯通

本轮不只实现单节点 TLS，也一并贯通：

- `RedissConnectionPool`
- TLS 版 master/slave client
- TLS 版 cluster client
- sentinel refresh
- slot refresh
- MOVED / ASK 跟随与自动重连

复用方式：

- 池化与 topology 内部都基于 `BasicRedisClient<SocketType>`
- seed / sentinel / node 地址之外，再携带 TLS 配置

## CMake 与模块对齐

参考 `galay-http` 调整：

- 根 `CMakeLists.txt` 先锁定 `galay-kernel`
- 复用顶层 `GALAY_BUILD_SSL`
- 启用时由 Redis 构建内部定义 `GALAY_SSL_FEATURE_ENABLED`
  - 不新增 Redis 专用公开 SSL 开关
  - `find_package(galay-ssl REQUIRED)`
  - 链接 `galay-ssl::galay-ssl`
  - 增加 TLS 编译宏
- `module_prelude.hpp` 去掉对 `Coroutine.h` 的探测
- 改为显式纳入：
  - `task.h`
  - `awaitable.h`
  - `galay-ssl/async/ssl_socket.h`
  - `galay-ssl/async/ssl_awaitable_core.h`
  - `galay-ssl/ssl/ssl_context.h`
- `galay_redis.cppm` 导出 TLS 客户端相关头

## 错误处理

### TCP 错误

- 继续映射为现有 `RedisErrorType`

### TLS 错误

- TLS handshake 失败、证书错误、peer closed、SSL send/recv 失败，需要稳定映射到 `RedisError`
- timeout 语义继续统一落到 `REDIS_ERROR_TYPE_TIMEOUT_ERROR`

### URL / 配置错误

- `rediss://` 缺省端口默认 6380
- SNI 默认取 URL host；显式配置的 `server_name` 优先
- 证书校验开启时，空 host / 无法设置 hostname 需要直接失败

## 测试策略

### 编译与表面

- `task.h` / 模块前导无 `Coroutine.h`
- `GALAY_BUILD_SSL=ON/OFF` 都可配置；Redis TLS facade 由内部 `GALAY_SSL_FEATURE_ENABLED` 控制
- `RedisCommandResultAwaitable` 不再存在
- topology 接口改成 `Task<...>`

### TCP 回归

- 现有单节点 / pipeline / timeout / pool / topology 测试继续通过

### TLS 功能

- `rediss://` connect/auth/select/command/batch/receive
- 握手失败
- 证书校验关闭 / 开启
- SNI 设置
- timeout
- peer close

### 端到端

- TLS connection pool smoke
- TLS sentinel refresh smoke
- TLS cluster refresh + MOVED/ASK smoke

## 验收标准

- Redis I/O 路径不再依赖仓库私有 awaitable 执行核心
- `RedisCommandResultAwaitable` 被移除
- `RedisClient` TCP 行为保持兼容
- `RedissClient` 可真实连接 TLS Redis
- TLS 版连接池与 topology 至少具备基础可用性
- README / docs / examples 同步体现 TLS 用法
