# C Async API Completion Plan

## 背景

当前仓库的 C 层实际构建目标主要是 `galay-c-kernel`。`test/c/CMakeLists.txt`
已经为 `galay::c-http`、`galay::c-ws`、`galay::c-http2`、`galay::c-redis`、
`galay::c-rpc`、`galay::c-mysql`、`galay::c-mongo`、`galay::c-etcd`、
`galay::c-mcp`、`galay::c-tracing` 预留了测试入口，但当前 `build/direct-coro-c`
只暴露 `galay-c-kernel` 目标。

因此，“C async 接口都封装完了吗”的当前结论是：kernel 的 direct C coroutine
async wrapper 已经覆盖较多；其它库还没有完整接入可构建的 C target，更没有完成
async client/server C ABI。

## 硬性规则

- C 头文件必须保持纯 C ABI，不暴露 C++ 类型、模板、异常或 namespace。
- C++ 生产代码禁止新增 `try` / `catch` / `throw`。
- C++ 可恢复错误通过 `std::expected` 或现有 typed error 返回；C API 通过结果结构体、
  明确错误码或状态字段返回。
- 每个公开 C 错误码枚举必须提供错误字符串函数。
- 所有非 `void` 返回值必须处理。
- 不新增返回 `Task` / `Task<T>` 的 helper；async C ABI 通过现有 kernel C coroutine
  bridge 或模块自己的 direct coroutine 边界实现。
- 禁止过分拆分 helper；关键错误传播、生命周期和资源释放逻辑保留在调用点附近。

## 总体架构

1. 每个 C 模块单独放在 `src/c/galay-<module>-c/`。
2. 每个模块提供一个真实 CMake target，例如 `galay-c-http` 和 alias `galay::c-http`。
3. 公共状态码继续复用 `galay_status_t`；模块特有错误补充模块枚举和 `*_get_error(...)`。
4. async API 以 `galay_coro_task_t` / C runtime 为执行边界，避免通过 spawn C++ task 旁路。
5. 同步/protocol helper 与 async client/server 分层：先保证协议 surface，再补网络 async。

## Phase 1: C target 与公共 ABI 基线

### 目标

补齐非 kernel C target 的真实构建入口，让现有 `test/c/*` surface 测试能够进入构建图。

### 模块

- `galay-c-common`
- `galay-c-utils`
- `galay-c-ssl`
- `galay-c-http`
- `galay-c-ws`
- `galay-c-http2`
- `galay-c-redis`
- `galay-c-rpc`
- `galay-c-mysql`
- `galay-c-mongo`
- `galay-c-etcd`
- `galay-c-mcp`
- `galay-c-tracing`

### 交付

- 每个模块至少有：
  - `CMakeLists.txt`
  - public C header
  - `.cc` wrapper implementation
  - `*_get_error(...)`
  - header smoke test
- 顶层 `src/c/CMakeLists.txt` 逐模块 `add_subdirectory(...)`。
- `ctest -L c` 能看到非 kernel C tests。

### 验证

```bash
rtk cmake --build build/direct-coro-c --target galay-c-http galay-c-redis galay-c-mysql -j2
rtk ctest --test-dir build/direct-coro-c -L c --output-on-failure
```

## Phase 2: HTTP / HTTP2 / WS C async API

### HTTP

实现：
- request/response/header/body builder C ABI。
- async client:
  - create/destroy
  - connect/close
  - send request
  - receive response
  - timeout 配置
- async server:
  - create/destroy
  - bind/listen/stop
  - route callback registration
  - request/response callback bridge

### HTTP2

实现：
- settings/frame/hpack 已有测试对应 surface 的真实 target。
- async h2/h2c client:
  - connect/close
  - send headers/data
  - recv headers/data
  - stream reset/goaway observation
- async h2/h2c server:
  - bind/listen/stop
  - stream callback
  - settings 配置与错误传播

### WS

实现：
- URL/config/frame helper C ABI。
- async client:
  - connect/close
  - send text/binary/ping/pong/close
  - recv frame
- async server:
  - upgrade callback
  - frame callback
  - close/error callback

### 验证

- C surface tests 覆盖 invalid argument、timeout、closed socket、oversized frame/header。
- loopback integration tests 使用 kernel C runtime 和 direct C coroutine。
- 不允许依赖 C++ exception fallback。

## Phase 3: Redis / MySQL / Mongo C async API

### Redis

实现：
- RESP command/reply helper。
- async standalone client:
  - connect/auth/select/close
  - command pipeline
  - recv reply
- async topology/cluster 最小只读 discovery 后续再扩展。

当前进度：
- 已补齐 standalone direct C coroutine 最小闭环：`connect`、单条 `command_async`、
  `close`，并通过本地 mock Redis loopback test、example 和 smoke benchmark 验证。
- 待补齐：auth/select、pipeline 独立 API、批量 reply 保留、topology/cluster discovery。

### MySQL

实现：
- config/auth/packet helper。
- async client:
  - connect handshake
  - query
  - read result/error packet
  - close

### Mongo

实现：
- URI/BSON/protocol helper。
- async client:
  - connect/hello
  - command
  - read reply
  - close

### 验证

- 本地 mock server loopback tests，不依赖外部数据库服务。
- protocol malformed/boundary tests 保留在 C 层。
- 每个 close/cleanup 返回值都必须处理。

## Phase 4: Etcd / MCP / RPC C async API

### Etcd

实现：
- config/endpoint/key-value helper。
- async client:
  - connect/close
  - get/put/delete/watch
  - lease 基础操作

### MCP

实现：
- JSON-RPC message/schema helper。
- async stdio/http transport client:
  - initialize
  - list tools/resources
  - call tool
  - close

### RPC

实现：
- endpoint/config/metadata/message helper。
- async client:
  - connect/call/stream/close
- async server:
  - bind/listen/register service/stop

### 验证

- Etcd/MCP 使用 fake transport 或 loopback mock，避免依赖外部服务。
- RPC 使用本地 C server/client E2E。

## Phase 5: Tracing / SSL C API 补齐

### SSL

实现：
- context/config/error C ABI。
- async SSL socket wrapper，复用 kernel C tcp wrapper。

### Tracing

实现：
- tracer/span/context/log sink C ABI。
- async exporter 仅在底层 C++ 已提供非阻塞路径时暴露；否则保持同步 surface。

## 提交节奏

每个 phase 独立提交，不把所有模块塞进一个大 commit。

推荐顺序：
1. `feat: 补齐 C 模块 target 基线`
2. `feat: 补齐 HTTP HTTP2 WS C async API`
3. `feat: 补齐 Redis MySQL Mongo C async API`
4. `feat: 补齐 Etcd MCP RPC C async API`
5. `feat: 补齐 SSL Tracing C API`

每次提交前必须：

```bash
rtk rg -n "try\\s*\\{|catch\\s*\\(|throw\\b|#include <exception>|\\(void\\)|static_cast<void>" src/c src/cpp test/c || true
rtk cmake --build build/direct-coro-c -j2
rtk ctest --test-dir build/direct-coro-c -L c --output-on-failure
```
