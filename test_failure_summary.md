# 全量测试运行结果汇总

**运行时间**: 2026-08-23  
**构建预设**: developer-full  
**ctest 总数**: 576 (运行 539, 跳过 37)

---

## 一、CTest 真实失败 (需修复)

### 1. c.ws.async_upgrade_loopback — SEGFAULT

- **文件**: `test/c/ws/t2_async_upgrade_loopback.c`
- **现象**: 运行 1.34s 后 SIGSEGV
- **GDB 定位**: `reactor_wait()` → `io_scheduler.c:353`，写协程 `write_coro` 指针无效
- **根因分析**: WS 连接关闭后，scheduler 线程仍在 epoll_wait 中访问已释放的 controller。`galay_c_tcp_socket_close` 中的 `galay_c_io_scheduler_unregister` 与 reactor 线程存在竞争条件
- **严重程度**: 高

### 2. config.tracing_options_surface — Timeout

- **文件**: `test/cpp/config/tracing_options_surface.cmake` + `test/cpp/config/CMakeLists.txt:57-65`
- **现象**: 测试超时（无显式 TIMEOUT 属性）
- **根因**: 该测试执行 2-4 次完整的 CMake configure + 可能的 build/install 周期，耗时较长。`config.build_type_option` 有 `TIMEOUT 60`，但此测试没有设置
- **修复建议**: 在 `test/cpp/config/CMakeLists.txt` 添加 `set_tests_properties(config.tracing_options_surface PROPERTIES TIMEOUT 300)`
- **严重程度**: 中

### 3. redis.t22.pool.source.boundaries — Failed

- **文件**: `test/cpp/redis/t22_pool_source_boundaries.cc:119-130`
- **输出**: `redis pool awaitable declarations must live in details/pool_awaitable.h: class RedissPoolInitializeAwaitable :`
- **根因**: 测试用 `"class RedissPoolInitializeAwaitable :"` 做子串匹配（要求冒号在同一行），但 `pool_awaitable.h:104-106` 中因多重继承将冒号放在了下一行：
  ```cpp
  class RedissPoolInitializeAwaitable
      : public galay::kernel::ForwardingAwaitable<RedissPoolInitializeAwaitable>
      , public galay::kernel::TimeoutSupport<RedissPoolInitializeAwaitable>
  ```
- **修复建议**: 将测试中的搜索串改为 `"class RedissPoolInitializeAwaitable"` （去掉尾部冒号），其他三个类的模式同理
- **严重程度**: 低（仅测试模式匹配格式问题）

---

## 二、CTest 预期跳过 (需要外部服务)

以下测试因缺少外部服务（Redis/MySQL/PostgreSQL/MongoDB/etcd）而跳过，属于预期行为：

| 标签 | 跳过数 | 说明 |
|------|--------|------|
| requires-redis | 9 | 需要 Redis 服务器 |
| requires-mysql | 10 | 需要 MySQL 服务器 |
| requires-postgres | 7 | 需要 PostgreSQL 服务器 |
| requires-etcd | 9 | 需要 etcd 服务器 |
| requires-mongo | 1 | 需要 MongoDB 服务器 |
| Disabled (http) | 5 | 显式禁用的 HTTP 测试 |

---

## 三、Benchmark 运行结果

### 自包含 Benchmark (无外部依赖)

| 类别 | PASS | TIMEOUT | FAIL | 说明 |
|------|------|---------|------|------|
| kernel (cpp) | 24 | 5 | 3 | timeout 为 server-only 类；fail 为 boost UDP loss / fair throughput / ownership WithTimeout |
| kernel (c) | 19 | 3 | 1 | timeout 为 server-only 类；fail 为 libuv 需要参数 |
| ssl | 3 | 0 | 2 | client/server 需要对端 |
| tracing | 11 | 0 | 0 | 全部通过 |
| utils | 10 | 2 | 0 | timeout 为长运行 benchmark |
| ws | 4 | 0 | 0 | 全部通过 |
| http | 6 | 0 | 0 | 全部通过 |
| http2 | 4 | 0 | 0 | 全部通过 |
| mcp | 9 | 1 | 1 | registration_move_pressure 超时；stdio 需要参数 |
| mongo | 3 | 0 | 0 | 全部通过 (crc32c / error boundaries / ownership) |

### Benchmark 失败详情

| Benchmark | 类型 | 原因 |
|-----------|------|------|
| benchmark_kernel_compare_boost_asio_coro_udp | FAIL | Boost.Asio UDP 丢包率 95%+，非 galay 问题 |
| benchmark_kernel_ownership_clone_pressure | FAIL | `WithTimeout<RecvAwaitable>` 构造丢失状态 |
| benchmark_kernel_tcp_socket_fair_throughput | FAIL | 公平性指标未达标 (loss 2.5%) |
| benchmark_c_kernel_libuv_echo_server | FAIL | 需要命令行参数 `<tcp|udp> <port>` |
| benchmark_ssl_tls_client_throughput | FAIL | 需要对端 server |
| benchmark_ssl_tls_server_throughput | FAIL | 需要证书路径参数 |
| benchmark_mcp_stdio_request_throughput | FAIL | 需要命令行参数 |
| benchmark_kernel_mpsc_unbounded_prefetch | TIMEOUT | 长运行 benchmark |
| benchmark_kernel_ring_buffer_throughput | TIMEOUT | 长运行 benchmark |
| benchmark_kernel_spsc_static_ring_throughput | TIMEOUT | 长运行 benchmark |
| benchmark_kernel_tcp_iov_server_throughput | TIMEOUT | server-only，需要 client |
| benchmark_kernel_tcp_server_throughput | TIMEOUT | server-only，需要 client |
| benchmark_kernel_udp_server_throughput | TIMEOUT | server-only，需要 client |
| benchmark_c_kernel_bounded_channel_throughput | TIMEOUT | 长运行 benchmark |
| benchmark_c_kernel_tcp_socket_server_throughput | TIMEOUT | server-only，需要 client |
| benchmark_c_kernel_udp_socket_server_throughput | TIMEOUT | server-only，需要 client |
| benchmark_utils_lru_cache_throughput | TIMEOUT | 长运行 benchmark |
| benchmark_mcp_registration_move_pressure | TIMEOUT | 长运行 benchmark |

---

## 四、Example 运行结果

| 类别 | PASS | FAIL | 说明 |
|------|------|------|------|
| kernel | 8 | 0 | 全部通过 |
| ssl | 0 | 4 | 需要运行中的 server / 证书参数 |
| tracing | 3 | 0 | 全部通过 |
| utils | 1 | 0 | 全部通过 |

SSL 示例失败均因需要对端 server 或证书文件路径参数，属于预期行为。

---

## 五、需要修复的问题汇总

| # | 问题 | 文件 | 严重程度 |
|---|------|------|----------|
| 1 | `c.ws.async_upgrade_loopback` SEGFAULT | `src/c/galay-kernel-c/core-c/io_scheduler.c:353` | 高 |
| 2 | `config.tracing_options_surface` 无 TIMEOUT | `test/cpp/config/CMakeLists.txt:57-65` | 中 |
| 3 | `redis.t22.pool.source.boundaries` 模式匹配失败 | `test/cpp/redis/t22_pool_source_boundaries.cc:122` | 低 |
