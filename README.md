# galay

galay 是一个基于 C++23 协程的高性能异步网络与协议框架，提供从运行时内核到各类协议客户端/服务端的一站式异步能力。

## 特性

- **C++23 协程运行时**：统一调度器、reactor（io_uring / epoll / kqueue）、task、channel、定时器。
- **全链路异步**：网络/文件 IO、TLS、协议解析、客户端连接池均基于协程，可 `co_await` 组合。
- **多协议支持**：HTTP/1.1、HTTP/2、WebSocket、TLS，以及 Redis / MySQL / PostgreSQL / MongoDB / etcd / RPC / MCP 等客户端。
- **可观测性**：内置 `tracing` 链路追踪模块（span、sampler、OTLP 导出、日志关联）。
- **结构体序列化**：通过独立 serde 依赖，字段注册一次即可进行 JSON/TOML 编解码，错误通过 `std::expected` 返回。
- **模块化构建**：C++ 模块位于 `src/cpp/galay-*`，C ABI 模块位于 `src/c/galay-*-c`，默认启用，可通过 `-DGALAY_BUILD_C_API=OFF` 关闭；同时支持 CMake 与 Bazel。
- **C++23 Modules**（可选）：在受支持的编译器上可启用 `galay_*` 模块目标。

## 模块

| 模块 | 说明 |
| --- | --- |
| `galay-kernel` | 协程运行时内核：Runtime、调度器、reactor、task、channel、定时器 |
| `galay-utils` | 通用工具：算法、缓存、配置、加密、编码、进程、熔断/限流/负载均衡 |
| `galay-serde` | 结构体与 JSON/TOML 转换，支持嵌套结构、容器和可选字段 |
| `galay-ssl` | 基于 OpenSSL 的异步 TLS：socket、上下文、握手 |
| `galay-http` | HTTP/1.1：server/client、路由、静态文件、chunk、range/etag、黑名单插件 |
| `galay-api` | 可选类型化 HTTP/1 REST：成员参数绑定、serde 契约校验、OpenAPI 3.1 与离线 Swagger UI |
| `galay-ws` | WebSocket：server/client、ws/wss、帧编解码 |
| `galay-http2` | HTTP/2：h2c/h2、多路复用、HPACK、流控 |
| `galay-redis` | Redis 客户端：异步、连接池、集群拓扑、TLS、pipeline/pubsub |
| `galay-rpc` | RPC 框架：一元/流式调用、服务发现 |
| `galay-mysql` | MySQL 客户端：异步、协议、认证、连接池、prepared、pipeline |
| `galay-postgres` | PostgreSQL wire protocol v3 客户端：同步/异步、SCRAM/MD5、prepared、事务、连接池、pipeline |
| `galay-mongo` | MongoDB 客户端：BSON、协议、pipeline、command、CRUD |
| `galay-etcd` | etcd 客户端：kv、lease、watch、sync/async |
| `galay-mcp` | MCP（Model Context Protocol）：server/client，stdio / http 传输 |
| `galay-tracing` | 链路追踪：span、sampler、exporter、OTLP、日志关联 |

## 环境要求

- 支持 C++23 的编译器（GCC 14+ / Clang 18+ / MSVC 2022 17.10+）
- CMake ≥ 3.28
- OpenSSL（`galay-ssl`、`galay-http2`、`galay-redis` 等 TLS 相关模块需要）

Galay 自带所需的 `concurrentqueue` 头文件。该副本位于
[`thirdparty/concurrentqueue`](thirdparty/concurrentqueue)，并在安装时放到
`include/galay/thirdparty/concurrentqueue`；构建和消费 Galay 不需要另外安装
或查找原始 `concurrentqueue` 包。

macOS 浮点文本转换使用随仓库分发的 `thirdparty/fast_float`（v8.0.2），解决
Apple libc++ 缺少浮点 `std::from_chars` 的问题。其他平台直接使用标准库，
不会引用或安装 fast_float；Bazel 仅在 macOS 目标上启用此依赖。

serde 通过 `thirdparty/serde` Git submodule 获取，MCP/etcd 共用其 JSON 后端。结构体转换示例与
三套构建说明见 [galay-serde](docs/cpp/modules/serde/00-快速开始.md)。

类型化 REST 与 API Docs 使用 `GALAY_BUILD_API=ON`（默认关闭），要求 HTTP 和
serde 同时开启。使用规则与范围见 [galay-api](docs/cpp/modules/api/00-快速开始.md)。

## 快速开始

```bash
# 默认构建通用模块、测试、示例与基准；galay-api 按需开启
cmake -B build
cmake --build build -j

# 运行测试
ctest --test-dir build --output-on-failure
```

按需关闭部分构建：

```bash
cmake -B build \
  -DGALAY_BUILD_BENCHMARKS=OFF \
  -DGALAY_BUILD_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF \
  -DGALAY_BUILD_POSTGRES=OFF \
  -DGALAY_BUILD_MONGO=OFF
```

启用 C++23 Modules（实验性）需要 CMake 3.31+，安装后的模块消费者同样需要
CMake 3.31+；包含 serde 时使用 Clang 17+ 或 GCC 15+ 与 Ninja：

```bash
cmake -B build -DGALAY_ENABLE_CPP23_MODULES=ON
```

## 目录结构

```
galay/
├── src/cpp/galay-*/ # C++ 功能模块源码
├── src/c/galay-*-c/ # C ABI 模块源码、common 与 bridge 层
├── examples/        # 各模块使用示例
├── test/            # 单元 / 集成测试（GoogleTest + CTest）
├── benchmark/       # 性能基准测试
├── thirdparty/      # 随 Galay 分发的第三方源码与许可证
├── docs/cpp/modules/# C++ 模块文档
├── docs/c/modules/  # C ABI 模块文档
├── cmake/           # CMake 选项、依赖与包配置
└── scripts/         # 辅助脚本
```

## 文档

每个模块的完整文档位于 [`docs/cpp/modules/`](docs/cpp/modules/)，覆盖快速开始、架构设计、API 参考、使用指南、示例代码、性能测试、高级主题与常见问题。

C ABI 文档位于 [`docs/c/modules/`](docs/c/modules/)，按模块对齐 `src/c/galay-*-c`，包括 `bridge`、`common`、`utils` 等共享层文档。

版本与发版记录见 [CHANGELOG.md](CHANGELOG.md) 与 [docs/release_note.md](docs/release_note.md)。

`v7.0.0` 将类型化 API 统一到 HTTP、HTTPS、h2c 和 H2 原生 Builder，删除独立的
`ApiBuilder` / `ApiServer` 等旧接口；MCP JSON 与 Schema 处理移交 serde。
消费者须迁移调用并重新编译，详见 [发布说明](docs/release_note.md)。

`v6.2.0` 将 Galay 自有函数统一为 `snake_case`，不保留旧名包装。消费者须同步修改
调用并重新编译，不可混用旧 C++ 共享库；命名对照、优化取舍与验证边界见
[重构评估](docs/cpp/modules/kernel/21-重构评估.md)。
