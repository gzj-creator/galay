# Etcd Client Direct Results Design

**Goal:** 把同步/异步 etcd client 从“`void/EtcdVoidResult + last*()` 缓存读取结果”改成“API 直接返回结构化结果”，并为异步 client 新增 `watch(key, Task)` 与 `watch(key, function)` 两个重载。

## 背景

当前 `EtcdClient` 与 `AsyncEtcdClient` 的公开契约都要求调用方在成功后继续读取 `lastKeyValues()`、`lastLeaseId()`、`lastDeletedCount()`、`lastPipelineResults()` 等最近结果缓存。这种接口有两个问题：

- 成功值不在函数返回值里，调用链可读性差。
- client 变成“共享状态缓存容器”，不利于组合调用，也不利于 watch 这种事件流 API 接入。

用户要求本轮改造直接对齐“返回最终结果”的风格，并且明确要求把 `last*` 公开接口直接移除。

## 设计目标

- 同步 `EtcdClient` 公开方法直接返回结构化 `std::expected<..., EtcdError>`。
- 异步 `AsyncEtcdClient` 的 awaitable 在 `await_resume()` 时直接返回相同语义的结构化结果。
- 完整移除公开 `last*` 访问器，不保留兼容 facade。
- 保留现有错误模型：失败仍统一通过 `std::expected` 的 error 分支表达。
- 新增异步 `watch` 能力，支持 task 处理器和普通函数处理器两个入口。

## 新的返回契约

同步客户端：

- `connect()` / `close()` / `put()` 返回 `EtcdBoolResult`
- `get()` 返回 `EtcdGetResult`
- `del()` 返回 `EtcdDeleteResult`
- `grantLease()` / `keepAliveOnce()` 返回 `EtcdLeaseGrantResult`
- `pipeline()` 返回 `EtcdPipelineResult`

异步客户端：

- `co_await connect()` / `close()` / `put()` 产出 `EtcdBoolResult`
- `co_await get()` 产出 `EtcdGetResult`
- `co_await del()` 产出 `EtcdDeleteResult`
- `co_await grantLease()` / `keepAliveOnce()` 产出 `EtcdLeaseGrantResult`
- `co_await pipeline()` 产出 `EtcdPipelineResult`

其中：

- `EtcdBoolResult = std::expected<bool, EtcdError>`
- `EtcdGetResult = std::expected<std::vector<EtcdKeyValue>, EtcdError>`
- `EtcdDeleteResult = std::expected<int64_t, EtcdError>`
- `EtcdLeaseGrantResult = std::expected<int64_t, EtcdError>`
- `EtcdPipelineResult = std::expected<std::vector<PipelineItemResult>, EtcdError>`

`bool` 返回值只承载“操作成功后的布尔语义”，不再额外通过 `lastBool()` 暴露：

- `put()` 成功返回 `true`
- `connect()` / `close()` 成功返回 `true`

## watch 设计

只在 `AsyncEtcdClient` 上新增 watch。

新增数据类型：

- `EtcdWatchEventType`
- `EtcdWatchEvent`
- `EtcdWatchResult`

约定：

- `watch(key, task_handler)`：启动一个内部协程循环，请求 `/v3/watch`，收到事件后把事件交给用户 task handler，并在 client 自己的 `IOScheduler` 上调度执行。
- `watch(key, function_handler)`：同样启动内部循环，但直接同步调用普通函数回调。
- 公开入口返回 `EtcdBoolResult`，表示 watch 启动是否成功；运行期错误通过内部循环停止并写日志，不再依赖 `lastError()`。

第一版只做单 key watch，不扩展 prefix/revision/start_revision/cancel 等选项。

## 实现落点

- `src/etcd/base/etcd_types.h`
  - 新增 pipeline/watch 结果类型。
- `src/etcd/base/etcd_internal.h`
  - 新增 watch request body 构造。
  - 新增 watch response/event 解析。
- `src/etcd/sync/etcd_client.h/.cc`
  - 改返回类型。
  - 删除 `last*` 访问器和对应状态缓存。
- `src/etcd/async/client.h/.cc`
  - 改 awaitable `Result` / `await_resume()` 返回值。
  - 删除 `last*` 访问器和对应状态缓存。
  - 新增 watch handler 类型与 watch 重载。
- `examples/`、`test/`、`docs/`
  - 全量改为直接使用返回值。

## 风险与控制

- 风险：同步/异步同时改动，编译面大。
  - 控制：先写 header surface 测试，再逐步改实现。
- 风险：watch 属于新协议路径，JSON 结构和普通 KV 请求不同。
  - 控制：先在 internal helper 层新增纯解析测试，再接入 async client。
- 风险：移除 `lastStatusCode()` / `lastResponseBody()` 后排障入口减少。
  - 控制：本轮按用户要求直接移除公开 `last*`，底层实现必要时保留内部局部变量，不再暴露为 API。
