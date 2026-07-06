# galay

galay 是一个基于 C++23 协程的高性能异步网络与协议框架，提供从运行时内核到各类协议客户端/服务端的一站式异步能力。

## 特性

- **C++23 协程运行时**：统一调度器、reactor（io_uring / epoll / kqueue）、task、channel、定时器。
- **全链路异步**：网络/文件 IO、TLS、协议解析、客户端连接池均基于协程，可 `co_await` 组合。
- **多协议支持**：HTTP/1.1、HTTP/2、WebSocket、TLS，以及 Redis / MySQL / MongoDB / etcd / RPC / MCP 等客户端。
- **可观测性**：内置 `tracing` 链路追踪模块（span、sampler、OTLP 导出、日志关联）。
- **模块化构建**：C++ 模块位于 `src/cpp/galay-*`，C ABI 模块位于 `src/c/galay-*-c`，按需启用；同时支持 CMake 与 Bazel。
- **C++23 Modules**（可选）：在受支持的编译器上可启用 `galay_*` 模块目标。

## 模块

| 模块 | 说明 |
| --- | --- |
| `galay-kernel` | 协程运行时内核：Runtime、调度器、reactor、task、channel、定时器 |
| `galay-utils` | 通用工具：算法、缓存、配置、加密、编码、进程、熔断/限流/负载均衡 |
| `galay-ssl` | 基于 OpenSSL 的异步 TLS：socket、上下文、握手 |
| `galay-http` | HTTP/1.1：server/client、路由、静态文件、chunk、range/etag、黑名单插件 |
| `galay-ws` | WebSocket：server/client、ws/wss、帧编解码 |
| `galay-http2` | HTTP/2：h2c/h2、多路复用、HPACK、流控 |
| `galay-redis` | Redis 客户端：异步、连接池、集群拓扑、TLS、pipeline/pubsub |
| `galay-rpc` | RPC 框架：一元/流式调用、服务发现 |
| `galay-mysql` | MySQL 客户端：异步、协议、认证、连接池、prepared、pipeline |
| `galay-mongo` | MongoDB 客户端：BSON、协议、pipeline、command、CRUD |
| `galay-etcd` | etcd 客户端：kv、lease、watch、sync/async |
| `galay-mcp` | MCP（Model Context Protocol）：server/client，stdio / http 传输 |
| `galay-tracing` | 链路追踪：span、sampler、exporter、OTLP、日志关联 |

## 环境要求

- 支持 C++23 的编译器（GCC 14+ / Clang 18+ / MSVC 2022 17.10+）
- CMake ≥ 3.20
- OpenSSL（`galay-ssl`、`galay-http2`、`galay-redis` 等 TLS 相关模块需要）

## 快速开始

```bash
# 默认开启全部模块、测试、示例与基准
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
  -DGALAY_BUILD_MONGO=OFF
```

启用 C++23 Modules（实验性）：

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
├── docs/cpp/modules/# C++ 模块文档
├── docs/c/modules/  # C ABI 模块文档
├── cmake/           # CMake 选项、依赖与包配置
└── scripts/         # 辅助脚本
```

## 文档

每个模块的完整文档位于 [`docs/cpp/modules/`](docs/cpp/modules/)，覆盖快速开始、架构设计、API 参考、使用指南、示例代码、性能测试、高级主题与常见问题。

C ABI 文档位于 [`docs/c/modules/`](docs/c/modules/)，按模块对齐 `src/c/galay-*-c`，包括 `bridge`、`common`、`utils` 等共享层文档。

版本与发版记录见 [CHANGELOG.md](CHANGELOG.md) 与 [docs/release_note.md](docs/release_note.md)。
