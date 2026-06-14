# Directory Convergence Design

## Goal

收敛 galay-utils 当前仍偏分散的公开头文件目录，让使用者能按清晰心智选择模块：

- `process/`：系统、进程、signal 和诊断边界。
- `cache/`：LRU 缓存、字节队列视图和环形缓冲。
- `tool/`：工程工具、线程池、等待器、对象池、流控、熔断和负载选择。
- `algorithm/`：算法和数据结构。

本设计不保留兼容头。每次移动公开头文件，都必须同步更新 umbrella header、module facade、测试、文档和安装验证。

## Target Layout

### `process/`

目标职责：OS / process / signal / crash diagnostic boundary.

- `system.hpp`
- `process.hpp`
- `signal.hpp`
- `backtrace.hpp`

理由：

- `SignalHandler` 本质上修改进程级 signal handler，放在 `process/` 比 `platform/` 更直观。
- `BackTrace` 和 crash handler 也服务于进程诊断边界。
- `System` 当前提供文件、目录、环境变量、主机信息等 OS convenience API，和进程边界一起归档更容易查找。

### `cache/`

目标职责：缓存与缓冲类工具。

- `lru_cache.hpp`
- `byte_queue_view.hpp`
- `ring_buffer.hpp`

理由：

- `LruCache`、`ByteQueueView`、`RingBuffer` 都围绕数据驻留、缓存和缓冲区组织，单独目录比并入通用 `tool/` 更容易检索。
- 这些类型保持无后台线程、无内部调度器语义；线程安全边界在 API 文档中说明。

### `tool/`

目标职责：可直接组合到业务系统里的工程工具。

- `thread.hpp`
- `pool.hpp`
- `rate_limiter.hpp`
- `circuit_breaker.hpp`
- `balancer.hpp`
- `balancer.inl`

理由：

- `ThreadPool`、`TaskWaiter`、`ObjectPool`、`BlockingObjectPool` 是工程资源工具，阻塞语义在 API 文档中说明，而不再保留单独公开目录。
- `RateLimiter`、`CircuitBreaker` 是运行期流控与保护工具。
- `Balancer` 是节点选择工具，虽然有算法属性，但业务使用心智更接近流量工具。

### `algorithm/`

目标职责：算法和数据结构。

- `consistent_hash.hpp`
- `trie.hpp`
- `huffman.hpp`
- `mvcc.hpp`

理由：

- `ConsistentHash` 更接近分布式哈希算法。
- `TrieTree`、`Huffman`、`Mvcc` 都是数据结构或算法结构，比单独 `data/` 更符合收敛目标。

### Keep As-Is

- `core/`：字符串、随机、时间、类型名等核心小工具。
- `common/`：底层定义、类型别名和宏。
- `app/`：CLI 应用支持。
- `config/`：配置解析。
- `crypto/`：MD5、MurmurHash3、Salt、HMAC。
- `encoding/`：Base64。
- `module/`：C++23 module facade。

## Migration Rules

- 不保留旧路径兼容头。
- 每个迁移批次都必须同步更新：
  - `galay-utils/galay_utils.hpp`
  - `galay-utils/module/galay_utils.cppm`
  - `test/import_smoke.cpp`
  - `test/CMakeLists.txt` 和相关测试 include
  - `README.md`
  - `docs/02-API参考.md`
  - `docs/03-使用指南.md`
  - `docs/04-示例代码.md`
  - `docs/06-高级主题.md`
  - `docs/07-常见问题.md`
  - `docs/plans/2026-06-06-module-production-roadmap.md`
- CMake 安装规则必须继续安装 `.hpp` 和 `.inl`。
- benchmark 和 examples 中的旧 include 必须同步迁移。
- 历史 release note 中的旧路径可保留为历史记录，不作为失败条件。

## Testing

每个批次至少运行：

```bash
rtk cmake --build cmake-build-test
rtk ctest --test-dir cmake-build-test --output-on-failure
rtk proxy git diff --check
```

迁移后还要扫描旧路径引用：

```bash
rtk proxy rg -n 'galay-utils/(platform|resilience|routing|data|buffer|cache)/' galay-utils test examples benchmark docs README.md CMakeLists.txt
```

该扫描允许历史 release note 或迁移说明中的旧路径引用，但不允许源码、测试、示例或当前 API 文档继续使用旧路径。

## Open Decisions

- `cache/` 保留为公开目录，承载 `LruCache`、`ByteQueueView` 与 `RingBuffer`。
- `routing/` 不保留，`balancer` 进 `tool/`，`consistent_hash` 进 `algorithm/`。
- `data/` 不保留，数据结构进入 `algorithm/`。
- `concurrency/` 不保留公开头文件，线程池、等待器和对象池进入 `tool/`，阻塞语义通过 API 文档和高级主题说明。
