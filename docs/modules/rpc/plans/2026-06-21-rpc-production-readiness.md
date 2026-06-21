# Galay RPC Production Readiness Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在暂不引入 proto 代码生成的前提下，把当前 galay-rpc 从可用异步 RPC 雏形推进到便于用户使用、性能可测、可接入配置中心和动态治理的生产化 RPC 基础设施。

**Architecture:** 先锁定协议、reader/writer、service surface、真实 loopback 和基准性能，避免后续治理改动没有回归保护。随后引入 `RpcChannel` 作为客户端连接级 I/O owner 和治理入口，再以版本化配置 snapshot 驱动 timeout、连接池、服务发现、流控、重试、熔断、观测和安全扩展。

**Tech Stack:** C++23, CMake/CTest, galay-kernel async runtime, galay-rpc RingBuffer/readv/writev, C ABI envelope codec, future etcd/config-center integration

---

## Scope

本计划不包含 proto 到 C++/C 的代码生成层，也不承诺兼容 gRPC wire protocol。目标是先完善当前 galay-rpc 自有协议和运行时治理能力。

## Current Capability Baseline

- 自定义 RPC envelope：magic/version/type/flags/request_id/body_len，覆盖 REQUEST/RESPONSE/STREAM_*。
- Unary client/server：当前 `RpcClient` 是单 socket；`RpcServer` 每连接协程处理请求。
- Call mode route：支持 `UNARY`、`CLIENT_STREAMING`、`SERVER_STREAMING`、`BIDI_STREAMING` 的一帧式 method slot。
- Stream session：`RpcStreamServer` 支持 STREAM_INIT/DATA/END/CANCEL，但当前每连接单活跃 stream。
- 高性能基础：RingBuffer + readv/writev + payload view，协议解析路径已有少拷贝基础。
- C ABI：当前只覆盖 envelope request/response 编解码。
- 服务发现：已有 registry/selector 抽象雏形，但未接入 `RpcClient` 调用路径。

## Production Gaps

### P0

- 客户端连接模型不足：没有 `RpcChannel`、连接池、健康检查、重连、预热、idle 回收、并发 outstanding 请求表和 response dispatcher。
- Timeout/cancel 不是 RPC 一等策略：当前依赖 awaitable timeout，缺 deadline、服务端可见上下文和取消传播。
- 流控/背压缺失：缺 connection/stream 级 `max_inflight_requests`、`max_inflight_bytes`、write queue 水位和超限快速失败。
- 配置中心未接入运行时：已有 builder 静态字段，但没有版本化 runtime snapshot、watch 和热更新字段分类。
- 安全缺位：没有 TLS/mTLS、auth metadata、ACL、证书热更新入口。

### P1

- 服务发现未和 client/server 集成，watch 更新不会驱动连接池摘除或新建连接。
- 重试、负载均衡、熔断、限流没有一等 policy，也没有调用路径集成。
- 观测性不足：缺 metrics、latency histogram、trace context、connection churn、retry/deadline 指标。
- 协议 metadata 不足：缺 deadline、trace、auth、compression、encoding 等标准 metadata 扩展点。
- 错误模型偏窄：缺 `UNAVAILABLE`、`RESOURCE_EXHAUSTED`、`CANCELLED`、`DEADLINE_EXCEEDED`、`UNAUTHENTICATED`、`PERMISSION_DENIED` 等治理语义。

### P2

- C ABI 只到 codec；如果要服务 C 用户，需要 C client/server 或明确声明非目标。
- `RpcServer` 与 `RpcStreamServer` 分裂，长期应统一 listener/connection dispatcher。
- 文档里有 retry、circuit breaker、client pool 示例，但真实 API 尚未内建，容易让用户误解能力边界。

---

## Phase 0: Test And Benchmark Baseline

**Goal:** 先把当前 RPC 行为锁住，确保后续 P0/P1 改动有协议、边界、E2E 和性能回归保护。

**Files:**
- Modify: `test/cpp/rpc/t1_protocol.cc`
- Modify: `test/c/rpc/t1_envelope_codec.c`
- Create: `test/cpp/rpc/t5_reader_writer_boundary.cc`
- Create: `test/cpp/rpc/t6_service_surface.cc`
- Create: `test/cpp/rpc/t7_unary_loopback.cc`
- Create: `test/cpp/rpc/t8_stream_loopback.cc`
- Modify: `benchmark/c/rpc/b1_envelope_codec_smoke.c`
- Modify: `benchmark/cpp/rpc/CMakeLists.txt`
- Create: `benchmark/cpp/rpc/b6_unary_loopback_latency.cc`
- Create: `benchmark/cpp/rpc/b7_unary_mode_route_cache.cc`
- Create: `benchmark/cpp/rpc/b8_stream_loopback_latency.cc`

**Step 1: Expand protocol and C ABI boundary tests**

Cover wrong message type, truncated body, short response body, over-limit body, bad header length, invalid call mode, empty service/method, overlong names, small output buffer, unknown error code mapping, and two-segment borrowed payload.

Run:
```bash
rtk proxy ctest --test-dir build-rpc-phase0 -R 'rpc.t1.protocol|c.rpc.envelope_codec' --output-on-failure
```

Expected: both tests pass.

**Step 2: Add reader/writer and service surface tests**

Cover split header, incomplete body, malformed response body, payload view over two iovec segments rejected, wrapped RingBuffer parsing, stream frame RingBuffer parsing, same method name registered under different call modes, wrong mode miss, and stream method names.

Run:
```bash
rtk proxy ctest --test-dir build-rpc-phase0 -R 'rpc.t5.reader.writer.boundary|rpc.t6.service.surface' --output-on-failure
```

Expected: both tests pass.

**Step 3: Add real loopback E2E tests**

Cover real `RpcServer` + `RpcClient` unary loopback for echo/reverse/length, missing service/method, four current one-frame call modes, and real `RpcStreamServer` + `RpcClient::createStream()` happy path and missing-route cancel.

Run:
```bash
rtk proxy ctest --test-dir build-rpc-phase0 -R 'rpc.t7.unary.loopback|rpc.t8.stream.loopback' --output-on-failure
```

Expected: both tests pass without long sleeps; readiness uses bounded connect retry.

**Step 4: Add benchmark smoke coverage**

Use CMake glob/foreach for C++ RPC benchmark registration:

```cmake
file(GLOB GALAY_RPC_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc")

foreach(benchmark_source IN LISTS GALAY_RPC_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    galay_add_rpc_benchmark("benchmark_rpc_${benchmark_name}" "${benchmark_source}")
endforeach()
```

Add smoke benchmarks for C envelope codec throughput, unary loopback latency, route lookup, and stream loopback latency.

Run:
```bash
rtk proxy ./build-rpc-phase0/benchmark/c/rpc/benchmark_c_rpc_envelope_codec
rtk proxy ./build-rpc-phase0/benchmark/cpp/rpc/benchmark_rpc_b6_unary_loopback_latency -n 100 -s 128
rtk proxy ./build-rpc-phase0/benchmark/cpp/rpc/benchmark_rpc_b7_unary_mode_route_cache -n 10000
rtk proxy ./build-rpc-phase0/benchmark/cpp/rpc/benchmark_rpc_b8_stream_loopback_latency -n 100 -s 128
```

Expected: all exit 0, print throughput/latency/error_rate, and error_rate is 0.

**Step 5: Full Phase 0 gate**

Run:
```bash
rtk proxy ctest --test-dir build-rpc-phase0 -R 'rpc' --output-on-failure
```

Expected: all RPC tests pass.

---

## Phase 1: RpcChannel And Connection Governance

**Goal:** 建立连接级 I/O owner，让用户不直接管理单 socket，并为并发 unary、连接池、健康检查和动态配置留出统一入口。

**Files:**
- Create: `src/cpp/galay-rpc/kernel/rpc_channel.h`
- Modify: `src/cpp/galay-rpc/kernel/rpc_client.h`
- Modify: `src/cpp/galay-rpc/kernel/rpc_conn.h`
- Test: `test/cpp/rpc/t9_channel_unary_concurrency.cc`
- Benchmark: `benchmark/cpp/rpc/b9_channel_unary_concurrency.cc`

**Implementation Tasks:**

1. Define `RpcChannelConfig` with endpoint, ring buffer size, max message size, max outstanding requests, connect timeout, idle timeout, and health check interval.
2. Implement one reader coroutine per connection that owns response reads and dispatches by `request_id`.
3. Implement pending request table with bounded size and deterministic cleanup on timeout, connection close, and cancellation.
4. Keep simple unary `RpcClient` behavior working; add `RpcChannel` as the preferred managed API.
5. Add bounded connect/reconnect and health state: `Idle`, `Connecting`, `Ready`, `TransientFailure`, `Shutdown`.

**Tests:**

- 100 concurrent unary calls through one channel return matching payloads.
- Out-of-order responses are dispatched to the correct waiter.
- Exceeding max outstanding requests returns `RESOURCE_EXHAUSTED`.
- Server close wakes all pending calls with transport error.
- Channel shutdown is idempotent and does not leave background tasks alive.

**Benchmarks:**

- Compare current serial unary loopback versus channel concurrent unary for p50/p95/p99 and qps.
- Add `--assert-error-rate` and `--assert-p99-us` to benchmark binary before using it in CI.

---

## Phase 2: Runtime Config Snapshot And Local Config Provider

**Goal:** 支持配置中心接入前先抽象版本化配置，让 timeout、连接、流控和治理策略可热更新。

**Files:**
- Create: `src/cpp/galay-rpc/kernel/rpc_config.h`
- Create: `src/cpp/galay-rpc/kernel/rpc_policy.h`
- Modify: `src/cpp/galay-rpc/kernel/rpc_channel.h`
- Test: `test/cpp/rpc/t10_config_snapshot.cc`

**Implementation Tasks:**

1. Define `RpcRuntimeConfig` with client, server, route policy, retry, deadline, flow control, discovery, security, and observability sections.
2. Define `ConfigProvider` interface returning immutable `std::shared_ptr<const RpcRuntimeConfig>` snapshots with monotonically increasing version.
3. Implement `StaticConfigProvider` and in-memory watch provider for tests.
4. Classify fields as hot reloadable or restart-required.
5. Make `RpcChannel` read policy from atomic snapshot per call.

**Tests:**

- Updating timeout policy affects the next call without restarting channel.
- Updating max outstanding requests changes fast-fail behavior.
- Invalid config is rejected and previous snapshot remains active.
- Snapshot reads are thread-safe under concurrent calls.

---

## Phase 3: Discovery And Config Center Integration

**Goal:** 把服务发现和配置中心接到 channel/policy，不再停留在独立示例层。

**Files:**
- Modify: `src/cpp/galay-rpc/kernel/rpc_discovery.h`
- Create: `src/cpp/galay-rpc/kernel/rpc_resolver.h`
- Create: `src/cpp/galay-rpc/kernel/rpc_config_center.h`
- Test: `test/cpp/rpc/t11_discovery_resolver.cc`

**Implementation Tasks:**

1. Define `RpcResolver` with `resolve(service)` and `watch(service, callback)`.
2. Adapt existing local registry/selector into resolver.
3. Add endpoint attributes: weight, zone, health, metadata, version.
4. Add prefix-watch contract for etcd/config-center provider; implementation can start with local fake provider and later map to real etcd.
5. Drive channel pool endpoint add/remove/update from resolver watch events.

**Tests:**

- Endpoint add causes new calls to use the new endpoint.
- Endpoint remove drains old connection and prevents new traffic.
- Weight update changes selection distribution within tolerance.
- Watch reconnect preserves last known good endpoints.

---

## Phase 4: Deadline, Retry, Load Balance, Circuit Breaker, Rate Limit

**Goal:** 把治理能力做成一等 policy，并统一从 config snapshot 和 interceptor 链路读取。

**Files:**
- Modify: `src/cpp/galay-rpc/kernel/rpc_policy.h`
- Create: `src/cpp/galay-rpc/kernel/rpc_interceptor.h`
- Modify: `src/cpp/galay-rpc/protoc/rpc_base.h`
- Modify: `src/cpp/galay-rpc/protoc/rpc_error.h`
- Test: `test/cpp/rpc/t12_governance_policy.cc`
- Benchmark: `benchmark/cpp/rpc/b10_governance_overhead.cc`

**Implementation Tasks:**

1. Add deadline metadata and server-side deadline visibility.
2. Add retry policy: max attempts, per-try timeout, total deadline, exponential backoff, jitter, retryable error codes, idempotency gate.
3. Add load balance strategies: round robin, weighted random, least in-flight.
4. Add circuit breaker states and half-open probes.
5. Add token-bucket rate limit per service/method.
6. Add client/server interceptor chain for auth, metrics, tracing, and governance hooks.

**Tests:**

- Deadline exceeded returns deterministic error and handler can observe deadline.
- Retry only runs for idempotent unary calls by default.
- Circuit opens after threshold and half-open recovery works.
- Rate limit returns `RESOURCE_EXHAUSTED`.
- Governance overhead benchmark records per-call added latency.

---

## Phase 5: Stream Backpressure And Unified Server Dispatcher

**Goal:** 让 stream 不只是可跑，还能在长连接和快发送方场景下可控，并减少普通 server 与 stream server 的使用分裂。

**Files:**
- Modify: `src/cpp/galay-rpc/kernel/rpc_stream.h`
- Modify: `src/cpp/galay-rpc/kernel/streamsvc.h`
- Modify: `src/cpp/galay-rpc/kernel/rpc_server.h`
- Test: `test/cpp/rpc/t13_stream_flow_control.cc`
- Benchmark: `benchmark/cpp/rpc/b11_stream_backpressure.cc`

**Implementation Tasks:**

1. Add per-stream and per-connection read/write watermarks.
2. Add stream credit/window or equivalent bounded queue protocol.
3. Define behavior for oversize stream frame, slow consumer, stream cancel, and connection close.
4. Unify listener/connection dispatch so users can register unary and stream methods on one server surface.

**Tests:**

- Fast producer cannot grow memory unbounded.
- Slow consumer triggers backpressure or controlled `RESOURCE_EXHAUSTED`.
- Cancel wakes blocked send/read.
- Multiple sequential streams on one connection work; concurrent stream support is either implemented or explicitly rejected with tested error.

---

## Phase 6: Observability And Security

**Goal:** 生产环境可定位问题、可接认证授权、可用 TLS/mTLS。

**Files:**
- Create: `src/cpp/galay-rpc/kernel/rpc_metrics.h`
- Create: `src/cpp/galay-rpc/kernel/rpc_tracing.h`
- Create: `src/cpp/galay-rpc/kernel/rpc_security.h`
- Modify: `src/cpp/galay-rpc/kernel/rpc_interceptor.h`
- Test: `test/cpp/rpc/t14_observability_security.cc`

**Implementation Tasks:**

1. Add metrics sink abstraction: request total, latency histogram, in-flight gauge, retry count, deadline exceeded, connection churn.
2. Add trace context metadata propagation.
3. Add TLS transport abstraction and mTLS config surface.
4. Add auth metadata and server-side auth interceptor.
5. Add certificate reload hook from config snapshot.

**Tests:**

- Metrics counters and histograms are emitted for success and error paths.
- Trace context crosses client/server boundary.
- TLS handshake failure is reported deterministically.
- Auth interceptor rejects missing/invalid credentials.

---

## Phase 7: Documentation, Examples, And CI Gates

**Goal:** 让用户能按文档正确使用真实能力，并让测试和压测持续约束后续改动。

**Files:**
- Modify: `docs/modules/rpc/02-API参考.md`
- Modify: `docs/modules/rpc/03-使用指南.md`
- Modify: `docs/modules/rpc/05-性能测试.md`
- Modify: `docs/modules/rpc/06-高级主题.md`
- Add/modify examples under `examples/` if the repository keeps RPC examples there
- Add CI script only if the repository already has matching script conventions

**Implementation Tasks:**

1. Remove or mark demo-only retry/circuit/client-pool snippets until production API exists.
2. Document `RpcChannel`, config snapshot, resolver, flow control, governance, metrics, and security.
3. Add benchmark JSON output and short smoke gates.
4. Add long-running stress benchmark instructions outside default CI.

**CI Smoke Gate:**

```bash
rtk proxy ctest --test-dir build-rpc-phase0 -R 'rpc' --output-on-failure
rtk proxy ./build-rpc-phase0/benchmark/cpp/rpc/benchmark_rpc_b6_unary_loopback_latency -n 100 -s 128 --assert-error-rate 0
rtk proxy ./build-rpc-phase0/benchmark/cpp/rpc/benchmark_rpc_b8_stream_loopback_latency -n 100 -s 128 --assert-error-rate 0
```

The `--assert-*` options should be implemented before enabling benchmark gates in CI.

---

## Execution Order

1. Finish and merge Phase 0 only after full RPC CTest and short benchmark smoke pass.
2. Implement Phase 1 before connection pool, retry, load balancing, or service discovery integration; otherwise concurrent calls have no safe response dispatch owner.
3. Implement Phase 2 before real config center integration; config center should feed snapshots, not mutate runtime fields directly.
4. Implement Phase 3 with local fake provider tests first, then add real etcd/config-center provider.
5. Implement governance policies only after channel and config snapshot are stable.
6. Add stream backpressure before advertising long-running/high-throughput stream usage.
7. Keep docs aligned with actual APIs in the same change that exposes each capability.

## Release Criteria

- All RPC CTest cases pass.
- C and C++ RPC benchmark smoke binaries build and exit 0.
- Benchmark output includes p50/p95/p99 or throughput plus error rate.
- New governance/config/discovery features have failure-injection tests, not only happy paths.
- No docs claim a capability before its public API and test coverage exist.
