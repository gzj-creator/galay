---
name: galay-framework
description: Use when building, integrating, or extending anything on the galay C++23 coroutine networking framework — servers/clients for HTTP/1.1, HTTP/2, WebSocket, TLS, Redis, MySQL, MongoDB, etcd, RPC, MCP, tracing, or its C ABI. Covers the runtime/Task model, include/link/build conventions, error-handling contracts, and per-module API entry points so an agent can start coding immediately.
---

# galay 框架使用指南

## 概述

galay 是一个基于 **C++23 协程** 的高性能异步网络与协议框架，从协程运行时内核到各类协议客户端/服务端一站式覆盖。所有 IO（网络/文件/TLS/协议）都是协程化的，可用 `co_await` 组合；错误通过 `std::expected<T, E>`（C++）或结果码/`C_IOResult`（C）**显式传播，不用异常**。

- 仓库根：`/Users/gongzhijie/Desktop/projects/git/galay`
- 两套 API：C++ 模块（`src/cpp/galay-*`，命名空间 `galay::*`）与 C ABI（`src/c/galay-*-c`，前缀 `galay_*`）。
- 完整文档：每个模块在 `docs/cpp/modules/<name>/`（`00-快速开始` / `01-架构设计` / `02-API参考` / `03-使用指南` / `04-示例代码` …），C 侧在 `docs/c/modules/<name>/`。
- 可运行示例：`examples/cpp/<module>/include/*.cc`（头文件模式）与 `examples/cpp/<module>/import/*.cc`（C++23 module 模式）；C 示例在 `examples/c/<module>/*.c`。

## 何时使用本 skill

- 在 galay 上写服务端 / 客户端 / 中间件，或新增协议处理逻辑。
- 需要正确的 include 路径、CMake 链接目标、命名空间、构建开关。
- 需要 Runtime / `Task<T>` / 调度器 / channel / 定时器等运行时原语的用法。
- 需要某个协议模块（http/http2/ws/redis/mysql/mongo/etcd/rpc/mcp/tracing/ssl）的 API 入口。
- 通过 C ABI 从 C 或其它语言 FFI 驱动框架。

## 参考文件（按需加载，勿一次性全读）

- **[references/cpp-api.md](references/cpp-api.md)** — 13 个 C++ 模块的公开类型、方法签名与最小示例。写 C++ 时按模块查这一份。
- **[references/c-api.md](references/c-api.md)** — C ABI 层的错误约定、runtime/coro 驱动模型、每模块 handle 与关键签名、完整 C 示例。写 C/FFI 时查这一份。

编码前先读本页“核心心智模型 + 约定”，再打开对应参考文件的目标模块小节；需要更深内容时才进 `docs/` 与 `examples/`。

## 核心心智模型

1. **Runtime 是入口。** 创建 `Runtime`（或 `RuntimeBuilder`），把根协程用 `blockOn` / `spawn` 提交上去。Runtime 管理 IO 调度器、compute 调度器、阻塞线程池。
2. **业务逻辑写成 `Task<T>` 协程。** IO 操作返回 awaitable，用 `co_await` 取 `std::expected<...>`；判 `if (!result)` 处理失败，`result.error().message()` 拿原因。
3. **不抛异常。** C++ 可恢复错误一律 `std::expected<T, E>`；C 一律 `galay_status_t` / `C_IOResult`。所有非 void 返回值必须处理（见仓库 `CLAUDE.md` 准则 5、6）。
4. **禁止把协程逻辑拆成返回 `Task`/`Task<T>` 的 helper**（会产生额外协程帧/调度点）；关键控制流留在调用点附近（准则 7）。
5. **客户端模块常见形态**：`XxxClientBuilder().scheduler(sched).build()` → `co_await client.connect(...)` → `co_await client.<op>(...)` → `co_await client.close()`。数据类客户端多为异步 + 连接池，部分提供 `sync/` 同步变体。
6. **服务端模块常见形态**：`XxxServerBuilder().host().port()...build()` → 注册 handler/router → `server.start(...)`。HTTP handler 签名 `Task<void>(HttpConn&, HttpRequest)`。

### 最小可运行骨架（kernel + TCP）

```cpp
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
using namespace galay::kernel;
using namespace galay::async;

Task<int> fetch() {
    auto sock = TcpSocket::create();                 // std::expected<TcpSocket, IOError>
    if (!sock) co_return -1;
    sock->option().handleNonBlock();
    if (auto c = co_await sock->connect(Host(IPType::IPV4, "127.0.0.1", 8080)); !c)
        co_return -1;
    co_await sock->send("ping", 4);
    char buf[256]{};
    auto n = co_await sock->recv(buf, sizeof(buf));   // std::expected<size_t, IOError>
    co_await sock->close();
    co_return n ? static_cast<int>(*n) : -1;
}

int main() {
    Runtime rt = RuntimeBuilder().ioSchedulerCount(1).build();
    auto r = rt.blockOn(fetch());                     // std::expected<int, RuntimeError>
    return r.value_or(-1) >= 0 ? 0 : 1;
}
```

## 约定速查

### include 前缀与命名空间

| 语言 | include 前缀 | 命名空间 / 前缀 |
| --- | --- | --- |
| C++ | `<galay/cpp/galay-<module>/...>` | `galay::kernel`、`galay::async`、`galay::http`、`galay::http2`、`galay::websocket`、`galay::redis`、`galay::mysql`、`galay::mongo`、`galay::etcd`、`galay::rpc`、`galay::mcp`、`galay::tracing`、`galay::utils`、`galay::ssl` |
| C | `<galay/c/galay-<module>-c/...>` | 函数 `galay_<module>_<action>()`、类型 `galay_<module>_<type>_t`、共享值类型 `C_Host`/`C_IOResult`/`C_RuntimeConfig` |

### CMake 链接目标

```cmake
find_package(galay-<module> CONFIG REQUIRED)          # 或整包
target_link_libraries(app PRIVATE galay::kernel galay::http)
```

- C++ target：`galay::kernel`、`galay::http`、`galay::http2`、`galay::ws`（注意 CMake target 是 `galay::ws`，但 C++ 命名空间是 `galay::websocket`）、`galay::redis`、`galay::mysql`、`galay::mongo`、`galay::etcd`、`galay::rpc`、`galay::mcp`、`galay::tracing`（附 `galay::tracing-kernel`、`galay::tracing-spdlog`）、`galay::utils`、`galay::ssl`。异步 socket 类型在 `galay::async` **命名空间**里，随 `galay::kernel` 提供，无独立 target。
- C target：`galay::c-kernel`、`galay::c-common`、`galay::c-bridge`、`galay::c-http`、`galay::c-http2`、`galay::c-ws`、`galay::c-redis`、`galay::c-mysql`、`galay::c-mongo`、`galay::c-etcd`、`galay::c-rpc`、`galay::c-mcp`、`galay::c-ssl`、`galay::c-tracing`、`galay::c-utils`。
- C++23 module 模式（`-DGALAY_ENABLE_CPP23_MODULES=ON`，需 CMake ≥ 3.28 + Ninja/VS + 非 AppleClang）：`import galay.kernel;`、`import galay.http;`、`import galay.http2;`、`import galay.websocket;`、`import galay.redis;`、`import galay.mysql;`、`import galay.mongo;`、`import galay.etcd;`、`import galay.rpc;`（`import galay.rpc.etcd;`）、`import galay.mcp;`、`import galay.tracing;`、`import galay.utils;`、`import galay.ssl;`。

### 构建开关（`cmake -B build -D<opt>=ON/OFF`）

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `GALAY_BUILD_KERNEL` / `_UTILS` / `_SSL` / `_HTTP` / `_HTTP2` / `_WS` / `_REDIS` / `_MYSQL` / `_MONGO` / `_ETCD` / `_RPC` / `_MCP` / `_TRACING` | ON | 逐模块开关 |
| `GALAY_BUILD_C_API` | ON | 构建 C ABI 层 |
| `BUILD_TESTING` / `GALAY_BUILD_EXAMPLES` / `GALAY_BUILD_BENCHMARKS` | 视预设 | 测试 / 示例 / 基准 |
| `GALAY_BUILD_SHARED_LIBS` | OFF | 动态库 |
| `GALAY_ENABLE_CPP23_MODULES` | OFF | 启用命名模块目标 |
| `GALAY_DISABLE_IOURING` | 平台相关 | ON→用 epoll；OFF 且有 liburing→io_uring |
| `GALAY_RPC_ENABLE_ETCD` | — | RPC 集成 etcd 服务发现 |
| `GALAY_TRACING_ENABLE_SPDLOG` / `_GALAY_HTTP_OTLP_TRANSPORT` | — | tracing spdlog sink / OTLP-over-HTTP 传输 |

标准构建与测试：

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

平台后端：Linux io_uring / epoll、macOS kqueue。示例里用 `USE_IOURING` / `USE_EPOLL` / `USE_KQUEUE` 宏切换调度器头文件（`core/uring_scheduler.h` / `core/epoll_scheduler.h` / `core/kqueue_scheduler.h`）。

## 模块地图

| 模块 | 用途 | C++ target | C++ 命名空间 | 参考 |
| --- | --- | --- | --- | --- |
| kernel | 协程运行时：Runtime、调度器、`Task<T>`、TCP/UDP/文件 IO、channel、mutex、定时器 | `galay::kernel`(+`galay::async`) | `galay::kernel` / `galay::async` | cpp-api.md |
| utils | 工具：字符串/编码/加密、缓存、算法、限流/熔断/负载均衡、配置 | `galay::utils` | `galay::utils` | cpp-api.md |
| ssl | 基于 OpenSSL 的异步 TLS：`SslContext` / `SslSocket` / 握手 | `galay::ssl` | `galay::ssl` | cpp-api.md |
| http | HTTP/1.1 server/client、路由、builder、静态文件、HTTPS | `galay::http` | `galay::http` | cpp-api.md |
| http2 | HTTP/2 h2c/h2、多路复用、HPACK、流控 | `galay::http2` | `galay::http2` | cpp-api.md |
| ws | WebSocket server/client、ws/wss、帧编解码 | `galay::ws` | `galay::websocket` | cpp-api.md |
| redis | 异步 Redis/Rediss 客户端、连接池、pipeline、pub/sub | `galay::redis` | `galay::redis` | cpp-api.md |
| mysql | 异步 MySQL 客户端、prepared、pipeline、事务、连接池 | `galay::mysql` | `galay::mysql` | cpp-api.md |
| mongo | 异步 MongoDB 客户端、BSON/OP_MSG、command、pipeline | `galay::mongo` | `galay::mongo` | cpp-api.md |
| etcd | 异步 etcd v3 客户端：kv、lease、watch、pipeline | `galay::etcd` | `galay::etcd` | cpp-api.md |
| rpc | RPC 框架：unary + 流式、服务端/客户端、服务发现 | `galay::rpc` | `galay::rpc` | cpp-api.md |
| mcp | Model Context Protocol：server/client，stdio/http 传输 | `galay::mcp` | `galay::mcp` | cpp-api.md |
| tracing | 链路追踪：Span、Sampler、OTLP 导出、日志关联 | `galay::tracing` | `galay::tracing` | cpp-api.md |
| C ABI | 上述全部模块的 C 封装 | `galay::c-*` | `galay_*` | c-api.md |

## 编码准则（本仓库强制，源自 `CLAUDE.md`）

1. **先思考再写**：不确定就提问；有多种解释就摆出来；有更简方案就指出。
2. **简洁优先**：用最少代码解决，不做投机性抽象/配置/错误处理。
3. **外科手术式修改**：只动必须动的，匹配现有风格，不顺手重构邻近代码。
4. **目标驱动**：把任务转成可验证目标，配合 TDD（先写失败测试）；测试放 `test/<module>`，压测/基准放 `benchmark/<module>`。
5. **异常禁用**：C++ 不新增 `throw`/`try`/`catch`；错误用 `std::expected` 传播。C 用结果结构体/错误码，且每个公开错误枚举都要有 `*_get_error(...)` 字符串函数。
6. **返回值必须处理**：所有非 void 返回值都要判断/传播；不得 void-cast 或裸调用丢弃。
7. **不过度拆 helper**：尤其不要把协程拆成返回 `Task`/`Task<T>` 的 helper；确需拆分先问用户。

写完代码后：按仓库 `code-review` 规范自检（可读性、函数 <50 行、文件 <800 行、错误处理、无硬编码密钥），并跑对应模块测试。
