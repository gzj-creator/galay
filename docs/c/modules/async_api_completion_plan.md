# C Async API 交付状态

## 当前结论

本文档原是补齐 C async API 的实施计划。截至 2026-08-15，计划中的模块
target、公开头文件、示例、测试和 benchmark 已进入当前构建图，因此这里只记录
现行契约和仍然存在的功能边界，不再把已完成的 phase 写成待办。

- kernel runtime、stackful coroutine、reactor、TCP/UDP、文件 I/O、同步原语和
  bounded channel 为原生 C 实现。旧 `galay-c-bridge` 已删除。
- 每个公开模块都位于 `src/c/galay-<module>-c/`，对应 CMake alias 为
  `galay::c-<module>`。
- 公开头文件和实现文件不使用 `_c` 文件名后缀；kernel 标识符使用
  `galay_c_*` 前缀。
- 原生 C MPMC/MPSC/SPSC 只提供 bounded channel；unbounded wrapper 和 token API
  不再属于 C ABI。

## C Async ABI 契约

- 公开头文件只暴露 C struct、枚举、opaque handle 和结果类型，不暴露 C++
  类型、模板、namespace、异常或 `Task<T>`。
- 异步 I/O 必须在 `galay_c_coro_spawn` 创建的 C 协程内调用，通过原生
  runtime 挂起和恢复，不在 scheduler 线程中做阻塞 I/O、阻塞锁或 sleep。
- 同步和生命周期 API 返回 `galay_status_t` 或模块错误枚举；协程 I/O
  返回 `C_IOResult`。每个公开错误枚举必须有覆盖全部枚举值的
  `*_get_error(...)`。
- create/acquire 必须有对应 destroy/release。借用 buffer、view、reply、row、
  frame 或 span 必须在公开头文件中说明失效时机。
- 所有非 `void` 返回值，包括 close、cleanup、rollback 和 stop，都必须
  检查、向上传播或合并为可观测失败。

## 模块与验收证据

| 模块 | 当前 surface | 主要验收入口 |
| --- | --- | --- |
| kernel | runtime/coroutine/reactor、TCP/UDP/file/AIO/watcher、mutex/waiter、bounded channel | `test/c/kernel/`、`benchmark/c/kernel/` |
| http | request/response/header helper，async client/server/session | `test/c/http/t3_async_client_server_loopback.c`、`t4_streaming_and_timeout.c` |
| http2 | frame/HPACK/settings/flow control，h2c client/server | `test/c/http2/t3_h2c_loopback.c` |
| ws | frame codec，async upgrade/frame I/O/close | `test/c/ws/t2_async_upgrade_loopback.c`、`t3_frame_io_and_close.c` |
| redis | standalone/auth/select/pipeline，pool lease，cluster route | `test/c/redis/t4_async_client_loopback.c` 至 `t8_cluster_route_loopback.c` |
| mysql | auth/result decode，query/stmt/transaction/pipeline/pool | `test/c/mysql/t3_async_client_loopback.c` 至 `t6_stmt_transaction_pool.c` |
| postgres | auth/query/result/stmt/transaction/pipeline/pool | `test/c/postgres/`、`docs/c/modules/postgres/05-性能测试.md` |
| mongo | BSON/URI/command builder，OP_MSG async client | `test/c/mongo/t2_bson_uri_client_surface.c` 至 `t5_op_msg_loopback.c` |
| etcd | sync/async KV，watch/lease/pipeline/cluster policy | `test/c/etcd/t2_sync_kv_surface.c` 至 `t5_cluster_policy_stats.c` |
| mcp | JSON-RPC helper，stdio/HTTP client，server handler | `test/c/mcp/t3_stdio_client_loopback.c` 至 `t5_server_handlers.c` |
| rpc | envelope/unary/streaming，pool/deadline/cancellation | `test/c/rpc/` |
| ssl | context，TLS handshake/send/recv/shutdown，ALPN/session | `test/c/ssl/` |
| tracing | context/span/provider/exporter/sampler/logger | `test/c/tracing/` |
| utils | bytes/ring buffer/Base64/digest | `test/c/utils/` |

模块 README 是功能和所有权边界的事实来源。表中“已有 surface”不等于与
对应 C++ SDK 完全对等；例如 MySQL 多结果集/LOCAL INFILE、PostgreSQL TLS/COPY/
LISTEN/NOTIFY、长稳外部服务矩阵仍按各模块 README 的 Deferred 边界处理。

## 验证

```bash
cmake --preset developer-full
cmake --build build/developer-full -j2
ctest --test-dir build/developer-full -L c --output-on-failure
```

窄化 kernel C 验证：

```bash
cmake --build build/developer-full \
  --target test_c_kernel_native_bounded_channels \
           benchmark_c_kernel_mpmc_bounded_channel_throughput -j2
ctest --test-dir build/developer-full -R '^c\.kernel\.' --output-on-failure
```

性能的现行 workload、Release 构建和结果有效性门槛见
[`kernel/05-性能测试.md`](./kernel/05-性能测试.md)。
