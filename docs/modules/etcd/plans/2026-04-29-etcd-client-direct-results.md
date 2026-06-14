# Etcd Client Direct Results Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让同步/异步 etcd client 直接返回结构化结果，移除全部 `last*` 公开接口，并新增异步 `watch(key, Task)` / `watch(key, function)` 两个重载。

**Architecture:** 先锁定新的公开 surface，再把同步和异步实现都切到“解析后直接返回值”的模式。watch 能力只落在 `AsyncEtcdClient`，底层仍复用现有 HTTP POST 发送路径，新增 watch JSON 构造与响应解析辅助函数。

**Tech Stack:** C++23, `std::expected`, `AsyncEtcdClient`, `IOScheduler`, `simdjson`, `galay-http`, `galay-kernel`

---

### Task 1: 锁定新的公开接口 surface

**Files:**
- Modify: `test/etcd/t7_await.cc`
- Create: `test/etcd/t9_api_surface.cc`
- Test: `test/etcd/t7_await.cc`

**Step 1: Write the failing test**

新增/修改编译期断言，要求：

- `EtcdClient` 和 `AsyncEtcdClient` 不再公开任何 `last*` 访问器
- 同步客户端方法返回新的 `std::expected<结构化值, EtcdError>`
- 异步 awaitable 的 `await_resume()` 返回新的结构化结果
- `AsyncEtcdClient` 公开 `watch(key, task_handler)` / `watch(key, function_handler)`

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target T7-EtcdAwaitableSurface -j8`

Expected: FAIL，因为头文件还暴露旧接口，返回类型和 watch surface 也还不匹配。

**Step 3: Write minimal implementation**

先只调整头文件声明到目标形态，暂时不保证链接通过。

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target T7-EtcdAwaitableSurface -j8`

Expected: PASS

**Step 5: Commit**

本任务不单独提交。

### Task 2: 重构同步客户端为直接返回值

**Files:**
- Modify: `src/etcd/sync/etcd_client.h`
- Modify: `src/etcd/sync/etcd_client.cc`
- Modify: `examples/etcd/include/e1_basic.cc`
- Modify: `examples/etcd/import/e1_basic.cc`
- Modify: `test/etcd/t1_smoke.cc`
- Modify: `test/etcd/t2_prefix.cc`
- Modify: `test/etcd/t3_pipe.cc`

**Step 1: Write the failing test**

把同步示例和测试改成直接消费返回值，例如：

- `auto get = client.get(key);`
- `auto kvs = get.value();`

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target T1-EtcdSmoke T2-EtcdPrefixOps T3-EtcdPipeline -j8`

Expected: FAIL，因为实现仍依赖旧缓存字段。

**Step 3: Write minimal implementation**

把同步实现改成解析成功后直接返回结构化值，并删除 `last*` 相关成员和方法。

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target T1-EtcdSmoke T2-EtcdPrefixOps T3-EtcdPipeline -j8`

Expected: PASS

**Step 5: Commit**

本任务不单独提交。

### Task 3: 重构异步客户端为直接返回值

**Files:**
- Modify: `src/etcd/async/client.h`
- Modify: `src/etcd/async/client.cc`
- Modify: `examples/etcd/include/e2_basic.cc`
- Modify: `examples/etcd/import/e2_basic.cc`
- Modify: `test/etcd/t4_smoke.cc`
- Modify: `test/etcd/t5_pipe.cc`

**Step 1: Write the failing test**

把异步示例和测试改成直接消费 `co_await` 返回值，例如：

- `auto kvs = co_await client.get(key);`
- `auto lease_id = co_await client.grantLease(3);`

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target T4-AsyncEtcdSmoke T5-AsyncEtcdPipeline -j8`

Expected: FAIL，因为 awaitable 仍只返回 `EtcdVoidResult`。

**Step 3: Write minimal implementation**

修改 awaitable `Result` 别名与 `await_resume()` 逻辑，让每类操作直接返回对应结果值。

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target T4-AsyncEtcdSmoke T5-AsyncEtcdPipeline -j8`

Expected: PASS

**Step 5: Commit**

本任务不单独提交。

### Task 4: 增加 async watch 类型、解析和公开入口

**Files:**
- Modify: `src/etcd/base/etcd_types.h`
- Modify: `src/etcd/base/etcd_internal.h`
- Modify: `src/etcd/async/client.h`
- Modify: `src/etcd/async/client.cc`
- Modify: `test/etcd/t6_helpers.cc`

**Step 1: Write the failing test**

新增纯 helper 测试和 surface 测试，要求：

- 能构造 watch request body
- 能解析 etcd watch 响应中的事件数组
- `AsyncEtcdClient` 存在两个 watch 重载

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target T6-EtcdInternalHelpers T7-EtcdAwaitableSurface -j8`

Expected: FAIL

**Step 3: Write minimal implementation**

实现：

- watch 结果类型
- watch JSON build / parse
- `AsyncEtcdClient::watch(key, task_handler)`
- `AsyncEtcdClient::watch(key, function_handler)`

**Step 4: Run test to verify it passes**

Run: `cmake --build build --target T6-EtcdInternalHelpers T7-EtcdAwaitableSurface -j8`

Expected: PASS

**Step 5: Commit**

本任务不单独提交。

### Task 5: 更新文档并做全量验证

**Files:**
- Modify: `README.md`
- Modify: `docs/01-架构设计.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md`
- Modify: `docs/06-高级主题.md`
- Modify: `docs/07-常见问题.md`

**Step 1: Write the failing test**

这里不新增自动化测试，直接把文档中的旧 API 叙述视为待修正项。

**Step 2: Run test to verify it fails**

Run: `rg -n "lastKeyValues\\(|lastDeletedCount\\(|lastLeaseId\\(|lastPipelineResults\\(|lastStatusCode\\(|lastResponseBody\\(|lastBool\\(|lastError\\(" README.md docs examples/etcd test/etcd src/etcd`

Expected: 仍能搜到旧公开 API 痕迹。

**Step 3: Write minimal implementation**

把文档、示例、测试统一改到新返回值契约，并补 watch 使用说明。

**Step 4: Run test to verify it passes**

Run:

- `rg -n "lastKeyValues\\(|lastDeletedCount\\(|lastLeaseId\\(|lastPipelineResults\\(|lastStatusCode\\(|lastResponseBody\\(|lastBool\\(|lastError\\(" README.md docs examples/etcd test/etcd src/etcd`
- `cmake --build build -j8`
- `ctest --test-dir build --output-on-failure`

Expected:

- 仓库源码和文档不再引用旧 `last*` API
- 全量构建成功
- 测试通过；依赖本地 etcd 的测试在服务可用时通过

**Step 5: Commit**

是否提交由用户单独决定，本任务不自动提交。
