# galay 协程客户端模块重构 — 执行文档

> 供独立 agent 执行。每个 Phase 自足：含精确文件、改动内容、风险点、验证命令。
> **执行顺序：Phase 0a → 2 → 3 → 4 → 6 → 7。**（Phase 0b 已完成，Phase 1 无操作；Phase 5 已按后续要求扩展并完成。）
> 铁律：禁异常/throw/try/catch；错误经 `std::expected` 传播；RAII；缓存行隔离用 `alignas(64)`。
> 每个 Phase 用自身 `-R` ctest 过滤器变绿后再进下一个。

---

## 背景与已确认决策

用户对 galay-etcd 接口不满意，诉求：接口易用性、命名空间统一、awaitable 拆文件。全仓探查结论：

- 命名空间**已基本统一**为 `namespace galay::xxx {`。唯一离群是 **galay-mcp**（15 文件仍旧式嵌套）。
- awaitable 拆分：**etcd 已最规范**（`details/awaitable.h`+`.inl`）。真正内联未拆的是 **mysql**。redis 已内联封装到位。
- 接口易用性真正痛点：cluster pool 只有非阻塞 `tryAcquire()`。
- 性能：本轮**只做缓存行对齐**，不查回归。

用户确认：**全部模块都可以动**、**加便捷高阶 API**、**性能只看缓存行对齐**。

仓库根：`/Users/gongzhijie/Desktop/projects/git/galay`

---

## 锁定契约（测试硬约束，任何 Phase 都不得破坏）

- `test/cpp/etcd/t7_await.cc` + `t10_http_source_boundaries.cc`：awaitable 必须在 `galay::etcd::details`；实现必须留在 `details/awaitable.inl`（不得进 `async/client.cc`）；awaitable 不是 `AsyncEtcdClient` 公有成员；`await_resume()` 返回类型精确匹配。**t10 只读 etcd 文件，且硬编码字面路径 `sync/etcd_client.cc`、`async/client.cc`** → 不锁 mysql/redis，但**禁止 etcd 文件改名**。
- `t17_ownership_surface.cc`：client move-only；builder 有 `.clone()`；lease move-only（无 clone）。
- `t18_cluster_connection_reuse.cc`：无锁池；`tryAcquire` 池空返回 `PoolExhausted`；lease 析构归还；8 线程并发安全；池被 move 后仍可用。
- `b5_ownership_move_clone.cc`、`b6_cluster_connection_reuse.cc`：池取/还吞吐基准，须继续通过。

> 注意：clang LSP 缺 build include 路径时会对这些文件报 `<optional>`/`std::nullopt` 找不到之类错误——那是**误报**，以 `cmake` 实际编译为准。

---

## Phase 0b — etcd 池 detail→details（✅ 已完成，勿重做）

已把 `cluster/etcd_cluster_client.{h,cc}` 内 `namespace detail`（单数）+ 全部 `detail::EtcdClientPoolState` 引用改为复数 `details`。校验命令（应输出 exit:1 = 无残留）：
```bash
grep -rn "\bdetail::EtcdClientPoolState\|namespace detail\b" src/cpp/galay-etcd/cluster/
```

---

## Phase 1 — etcd 文件命名（无操作，勿改）

`sync/etcd_client.{h,cc}` 与 `async/client.{h,cc}` 命名不对称属外观问题。改名会波及 `module/galay_etcd.cppm`、所有测试 include、CMake GLOB，且 **t10 硬编码字面路径** → 破坏字符串断言。**结论：保持现状。**

---

## Phase 0a — galay-mcp 命名空间旧式→新式（15 文件）

**目标**：把旧式 `namespace galay {` + `namespace mcp {` 改为 C++17 `namespace galay::mcp {`，两个闭合花括号合并为一个 `} // namespace galay::mcp`。**不改任何符号名。**

**定位待改文件**（精确列表，逐个处理）：
```bash
grep -rln "namespace galay {" src/cpp/galay-mcp/
```
预期 15 个：`common/{mcp_error,mcp_json,mcp_base,json_parser}.{h,cc}`、`common/{schema_builder,protocol_utils,mcp_policy}.h`、`server/{stdio_server,http_server}.{h,cc}`（以 grep 实际输出为准）。

**每个文件的机械改法**：
1. 打开文件，找到 `namespace galay {`（下一行或附近是 `namespace mcp {`），替换为单行 `namespace galay::mcp {`，删掉多出的那层 `{`。
2. 文件尾对应的两个闭合注释（形如 `} // namespace mcp` + `} // namespace galay`）合并为一个 `} // namespace galay::mcp`。
3. 若中间还有其它嵌套（如 `namespace detail`），保持不动。

**注意**：部分文件可能已是新形式或含 `client/*`（已新形式）——只改 grep 命中的文件。逐文件 Read 确认嵌套结构后再 Edit，勿盲改。

**风险**：低。不改符号，仅命名空间书写形式。`galay_mcp.cppm` 无需改（include 同一批头）。

**验证**：
```bash
grep -rln "namespace galay {" src/cpp/galay-mcp/   # 应无输出（全部转新式）
cmake --build build -j --target galay-mcp 2>/dev/null || cmake --build build -j
ctest --test-dir build --output-on-failure -R "mcp"
```

---

## Phase 2 — 池 state 缓存行对齐（alignas 64）

**约定**：纯 `alignas(64)`（见 `src/cpp/galay-kernel/concurrency/mpsc_channel.h:386`、`common/balancer.hpp:95`）。把两个热 atomic 与队列隔离到独立缓存行，冷成员分组。**不改队列语义、不加 mutex。**

**2a. 同步 `details::EtcdClientPoolState`**（`src/cpp/galay-etcd/cluster/etcd_cluster_client.cc`，结构体约在 274-346 行）
把成员声明改为：
```cpp
    alignas(64) moodycamel::ConcurrentQueue<EtcdClient*> idle_clients;   // producer/consumer 热路径
    alignas(64) std::atomic<size_t> borrowed_count{0};                   // acquire fetch_add / release fetch_sub
    alignas(64) std::atomic<bool>   queue_failed{false};                 // 极少写，隔离
    std::optional<EtcdError> init_error;                                 // 冷
    std::vector<std::unique_ptr<EtcdClient>> clients;                    // 冷
```
> 现有声明顺序是 `idle_clients; init_error; clients; borrowed_count; queue_failed;`。改成上面的顺序（热 atomic 上提到队列后、冷成员前），并给 `idle_clients`/`borrowed_count`/`queue_failed` 加 `alignas(64)`。**同步检查构造函数成员初值列表顺序**必须与新声明顺序一致（`idle_clients` 先初始化——已满足；`borrowed_count{0}`/`queue_failed{false}` 用类内默认初值即可，不必进初值列表）。

**2b. 异步 `details::AsyncEtcdClientPoolState`**（`src/cpp/galay-etcd/async/client.cc`，结构体约在 490-565 行）
同样处理，冷组额外含 `galay::kernel::IOScheduler* scheduler`：
```cpp
    alignas(64) moodycamel::ConcurrentQueue<AsyncEtcdClient*> idle_clients;
    alignas(64) std::atomic<size_t> borrowed_count{0};
    alignas(64) std::atomic<bool>   queue_failed{false};
    std::optional<EtcdError> init_error;                                 // 冷
    std::vector<std::unique_ptr<AsyncEtcdClient>> clients;               // 冷
    galay::kernel::IOScheduler* scheduler = nullptr;                     // 冷（构造后不变）
```
> 现有构造函数初值列表 `: idle_clients(...), scheduler(owner_scheduler)`——`scheduler` 改到冷组后，**初值列表里 `scheduler(...)` 的位置须挪到 `idle_clients(...)` 之后且不早于其声明**；因 `scheduler` 声明现在排在最后，初值列表顺序 `idle_clients` 然后 `scheduler` 仍合法（初值列表顺序须 ≤ 声明顺序，`idle_clients`(第1) → `scheduler`(第6) 合法）。编译若报 `-Wreorder` 按声明顺序调整初值列表即可。

`<atomic>` 两个 `.cc` 已包含。struct 经 `make_shared` 堆分配，`alignas` 撑大无碍。确认 `idleCount()`/`size()`（读 `borrowed_count`/`clients.size()`）仍编译。

**风险**：仅内存布局变化，API 不变。b5/b6/t18 并发路径与 `idleCount()` 须回归通过。

**验证**：
```bash
cmake --build build -j
ctest --test-dir build --output-on-failure -R "t18_cluster_connection_reuse|b5_ownership_move_clone|b6_cluster_connection_reuse"
```

---

## Phase 3 — mysql awaitable 拆分到 details/（镜像 etcd）

**目标**：把 5 个 awaitable 模板移出 928 行的 `src/cpp/galay-mysql/async/client.h`，保持 header-only 的 `.h`+`.inl` 拆分，client 里 type-alias。**mysql 已有显式实例化（`async/client.cc` 末尾 `template class ...<Mmap/Vector/Auto>`，约 2159-2176），拆分只是搬家，保留该机制。**

**先精确定位**（行号会因前序 Phase 微移，务必用 grep 重新定位）：
```bash
grep -n "class Mysql.*Awaitable\|template<RingBufferBackendStrategy\|template class Mysql\|namespace galay::mysql\|friend class Mysql" src/cpp/galay-mysql/async/client.h src/cpp/galay-mysql/async/client.cc
```
5 个类：`MysqlConnectAwaitable`、`MysqlQueryAwaitable`、`MysqlPrepareAwaitable`、`MysqlStmtExecuteAwaitable`、`MysqlPipelineAwaitable`（client.h 约 170-755）。模板体在 client.cc 约 191-2009；匿名命名空间 helper 约 client.cc:12-190。

**新文件 1：`src/cpp/galay-mysql/details/awaitable.h`**
```cpp
#ifndef GALAY_MYSQL_DETAILS_AWAITABLE_H
#define GALAY_MYSQL_DETAILS_AWAITABLE_H
#include "../async/client.h"   // 取 AsyncMysqlClient fwd + config/protocol/Strategy 类型
namespace galay::mysql::details {
// 5 个类模板定义（从 client.h 原样搬来），保留
//   template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
// 默认参数；类名/成员/friend 保持不变。
}
#endif
```

**新文件 2：`src/cpp/galay-mysql/details/awaitable.inl`**
- 把 client.cc 里 5 个模板的方法体逐字搬入，每个定义前加 `galay::mysql::details::` 限定（或整体包在 `namespace galay::mysql::details { ... }`）。
- awaitable 定义依赖的匿名命名空间 helper（client.cc:12-190）一并搬到 `.inl` 顶部（或提取到共享 details 头），保证定义可见。
- **`.inl` 只能被 client.cc 一个 TU include**（避免 ODR/重复符号）。

**改 `src/cpp/galay-mysql/async/client.h`**：
1. 删掉 5 个内联类定义（170-755）。
2. 在 `AsyncMysqlClient` 定义**之前**加 fwd decl：
   ```cpp
   namespace details {
   template<RingBufferBackendStrategy Strategy> class MysqlConnectAwaitable;
   template<RingBufferBackendStrategy Strategy> class MysqlQueryAwaitable;
   // ...其余 3 个
   }
   ```
3. 在 `AsyncMysqlClient` 内加公有 type-alias（镜像 etcd `async/client.h:82-90`）：
   ```cpp
   using ConnectAwaitable = details::MysqlConnectAwaitable<Strategy>;
   // ...其余 4 个
   ```
   （若 `AsyncMysqlClient` 本身是模板则用其 `Strategy` 参数；若不是，用默认 `Mmap` 或按原公有别名写法对齐。**以原 client.h 已有的公有暴露方式为准**。）
4. 保留 `friend class` 声明（原 900-904）。
5. 在**文件末尾、`AsyncMysqlClient` 完整定义之后**加 `#include "../details/awaitable.h"`（模板经 friend 访问 client 内部，顺序陷阱——必须在完整定义后）。

**改 `src/cpp/galay-mysql/async/client.cc`**：
1. 删已搬走的模板体（191-2009）和搬走的匿名 helper（12-190，若已移到 `.inl`）。
2. **保留** `AsyncMysqlClient<Strategy>` 成员实现（约 2011-2157）与显式实例化块（约 2159-2176）。
3. 在 client.h include 之后加 `#include "../details/awaitable.inl"`。

**CMake/module**：`src/cpp/galay-mysql/CMakeLists.txt` 用 `GLOB_RECURSE *.cc`，`.h`/`.inl` 不直接编译 → **无需改**。`galay_mysql.cppm` 无需改。

**风险点**：
- `.inl` 只被 client.cc include（否则重复符号）。
- `awaitable.h` 必须在 `AsyncMysqlClient` 完整定义后 include。
- 保留 Strategy 默认参数与三份显式实例化，否则链接缺符号。

**验证**：
```bash
cmake --build build -j
ctest --test-dir build --output-on-failure -R "mysql"
```

---

## Phase 4 — pool 便捷高阶 API（纯新增）

全部 additive，`tryAcquire()` 不变，不 throw。归还由 RAII lease 析构保证。

**4a. 同步 `EtcdClusterClient`（`src/cpp/galay-etcd/cluster/etcd_cluster_client.h/.cc`）** 新增两个方法：
```cpp
// 头文件声明
[[nodiscard]] EtcdClientAcquireResult acquireConnected();   // acquire + 确保已连接

template <class Fn>   // Fn: EtcdClient& -> std::expected<T, EtcdError>
[[nodiscard]] auto withClient(Fn&& fn)
    -> std::expected<
         typename std::invoke_result_t<Fn, EtcdClient&>::value_type,
         EtcdError>;
```
`withClient` 头内联实现（模板）：
```cpp
template <class Fn>
auto EtcdClusterClient::withClient(Fn&& fn)
    -> std::expected<typename std::invoke_result_t<Fn, EtcdClient&>::value_type, EtcdError>
{
    auto lease = tryAcquire();
    if (!lease.has_value()) {
        return std::unexpected(lease.error());
    }
    // 可选：若 lease->connected() 为 false 先 connect；connect 失败则 return unexpected
    return std::forward<Fn>(fn)(**lease);   // lease 作用域末析构 → 保证归还（含早返回）
}
```
`acquireConnected()` 放 `.cc`：`tryAcquire()`→若未连接则 `lease->connect()`→失败 `return unexpected(err)`，成功 `return lease`。

**4b. 异步 `AsyncEtcdClusterClient`（`src/cpp/galay-etcd/async/client.h/.cc`）** — `withClient` 本身可 await，callable 返回 `Task<...>`：
```cpp
template <class Fn>
[[nodiscard]] auto withClient(Fn&& fn) -> galay::kernel::Task<
    std::expected< /* fn 返回的 Task<expected<T,E>> 里的 value_type */ , EtcdError>>;
```
头内联实现为 `Task` 协程：
```cpp
auto lease_res = tryAcquire();
if (!lease_res.has_value()) co_return std::unexpected(lease_res.error());
auto lease = std::move(*lease_res);           // ★ move 进协程 frame 局部，保证任何 co_return 都归还
auto conn = co_await lease->connect();
if (!conn.has_value()) co_return std::unexpected(conn.error());
co_return co_await std::forward<Fn>(fn)(*lease);
```
> 结果类型推导：`fn(*lease)` 返回 `Task<std::expected<T, EtcdError>>`。需一个 `Awaited<>` trait 取出 `co_await` 后的类型再取 `::value_type`。若仓库已有类似 trait 优先复用；否则在 `details` 里写一个最小 `Awaited`。**无 try/catch**——错误全是 `expected` 值。

**4c. 同步非 cluster `EtcdClient` 池** — 若存在同名 pool 类型，镜像 4a 加同样两方法。（先确认 `EtcdClient` 自身是否有池入口，无则跳过。）

**4d. Builder 去重** — **本轮跳过**（仅记录）。async builder 多 `scheduler()`/`bufferSize()`/`keepAlive()`，改 `clone()` 协变有碰 t17 契约风险。

**风险点**：
- t17 要求 builder 保留 `.clone()`、lease move-only → 新方法**不得复制 lease**（`withClient` 只返回操作结果或 `EtcdError`）。
- 异步 `withClient` 必须让 lease 跨 `co_await` 存活（frame 局部，非临时量）。
- 不得移除/改动 `tryAcquire()` 与 lease RAII 语义（t18）。

**验证**：见 Phase 6（t19 覆盖这些方法）。

---

## Phase 5 — 其余客户端/连接池 awaitable 拆分（✅ 已完成）

后续要求将审计结论扩展为实际拆分，范围限定在客户端和连接池拥有的自定义 awaitable；协议内核中与连接/流对象紧耦合的局部 operation 保持原边界。

- Redis/Rediss 客户端：`src/cpp/galay-redis/details/awaitable.{h,inl}`
- Redis/Rediss 池：`src/cpp/galay-redis/details/pool_awaitable.{h,inl}`
- MySQL 池：`src/cpp/galay-mysql/details/pool_awaitable.{h,inl}`
- HTTP/2 TLS 客户端：`src/cpp/galay-http2/details/h2_client_awaitable.{h,inl}`
- HTTP/2 h2c 客户端：`src/cpp/galay-http2/details/h2c_client_awaitable.{h,inl}`
- RPC 客户端：`src/cpp/galay-rpc/details/client_awaitable.{h,inl}`

原 `async/redis_client.{h,cc}`、`async/conn_pool.{h,cc}` 与 MySQL `async/conn_pool.{h,cc}` 只保留前置声明、公开接口及单一 include。MySQL 池保留原嵌套类型身份 `MysqlConnectionPool::AcquireAwaitable` / `LeaseAwaitable`，不改 ABI 符号。

**单一 TU 约束**：

- `galay-redis/details/awaitable.inl` 只由 `async/redis_client.cc` include。
- `galay-redis/details/pool_awaitable.inl` 只由 `async/conn_pool.cc` include。
- `galay-mysql/details/pool_awaitable.inl` 只由 `async/conn_pool.cc` include。

HTTP/2 与 RPC 继续支持任意 `Strategy` / `SocketType` 模板实例，因此不采用单一 TU 实例化：对应 `details/*.h` 在文件末包含 `*.inl`，公开 client 头只保留前置声明、接口与 details include。HTTP、Mongo、MCP 仅暴露 `Task`/底层 socket awaitable，WebSocket 使用协议 upgrade operation，无独立命名的客户端 Awaitable 类，不新建空壳 details 类型。

**验证**：
```bash
cmake --build build -j
ctest --test-dir build --output-on-failure -R "redis.t22.pool.source.boundaries|mysql.t14.pool.coroutine.source|http2.client_awaitable_source|rpc.t105.client.awaitable.source"
```

---

## Phase 6 — 新增测试 + 文档

**既有测试不改、须继续通过**：etcd t7/t10/t17/t18、b5/b6。

**新增 `test/cpp/etcd/t19_pool_convenience.cc`**（gtest，sync + async 各覆盖）：
1. `withClient` happy-path：返回操作结果，值正确。
2. release-on-error：传入返回 `std::unexpected(...)` 的 op，调用后 `idleCount()` 回满（证明错误路径 RAII 归还）。
3. pool-exhausted：先用 lease 抽干池，再调 `withClient`，返回 `EtcdErrorType::PoolExhausted`，不死锁、不抛。
4. `acquireConnected`：无需手动 connect 即返回已连接 lease（或返回 connect 错误），`idleCount()` 相应下降。
> 参考既有 `test/cpp/etcd/t18_cluster_connection_reuse.cc` 的池构造与断言风格。async 用例需 Runtime/IOScheduler + `scheduleTask`，参考 `t14_async_cluster_integration.cc`。

**注册**：在 `test/cpp/etcd/CMakeLists.txt` 加 t19（对齐既有 test target 定义方式）。

**文档**：
- `docs/cpp/modules/etcd/02-API参考.md`：加 `withClient`/`acquireConnected` 签名（sync + async），注明**保证归还** + `std::expected` 错误面 + `tryAcquire` 仍可用。
- `docs/cpp/modules/etcd/03-使用指南.md`：加 `withClient` 高阶池示例，手动 acquire→connect→op→release 保留为“进阶”路径。

**验证**：
```bash
cmake --build build -j
ctest --test-dir build --output-on-failure -R "t19_pool_convenience"
```

---

## Phase 7 — 构建 + 全量验证

```bash
cd /Users/gongzhijie/Desktop/projects/git/galay
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j
cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" && cmake --build build-asan -j
```
分组 ctest（先锁定契约，再新增/受影响）：
```bash
ctest --test-dir build --output-on-failure -R "t7_await|t10_http_source_boundaries|t17_ownership_surface|t18_cluster_connection_reuse|t19_pool_convenience"
ctest --test-dir build --output-on-failure -R "b5_ownership_move_clone|b6_cluster_connection_reuse"   # 对齐回归
ctest --test-dir build --output-on-failure -R "mysql"
ctest --test-dir build --output-on-failure -R "redis|mcp"
ctest --test-dir build --output-on-failure                                                            # 全量
```
每个 Phase 用自身 `-R` 过滤器变绿后再进下一个。

---

## 关键文件速查
| Phase | 文件 |
|---|---|
| 0a | `src/cpp/galay-mcp/{common,server}/*`（grep 命中的 15 个） |
| 0b ✅ | `src/cpp/galay-etcd/cluster/etcd_cluster_client.{h,cc}` |
| 2 | `src/cpp/galay-etcd/cluster/etcd_cluster_client.cc`、`async/client.cc`（两个 PoolState 结构体） |
| 3 | `src/cpp/galay-mysql/async/client.{h,cc}` + 新增 `details/awaitable.{h,inl}` |
| 4 | `src/cpp/galay-etcd/cluster/etcd_cluster_client.{h,cc}`、`async/client.{h,cc}` |
| 6 | 新增 `test/cpp/etcd/t19_pool_convenience.cc`、`test/cpp/etcd/CMakeLists.txt`、`docs/cpp/modules/etcd/0{2,3}-*.md` |
