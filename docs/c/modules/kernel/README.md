# 文档导航

## 当前基线

- 真相优先级：公开头文件 > 实现 > `examples/c` > `test/c` > `benchmark/c` > Markdown
- 当前 C 文档按 C++ 文档的模块路径和编号口径组织，落点为 `docs/c/modules/kernel`
- 当前已验证环境：macOS / AppleClang 17 / Release 构建 / kqueue
- 当前已落地页面：`docs/c/modules/kernel/05-性能测试.md`
- 当前验证范围：C Kernel runtime、TCP/UDP socket、AsyncFile、AioFile、FileWatcher、AsyncMutex、AsyncWaiter、MpscChannel、UnsafeChannel 的 C ABI wrapper、测试、示例和 benchmark smoke

## 两层规则

- 先回答：优先在 `docs/c/modules/kernel/05-性能测试.md` 内给出当前可复现的 benchmark 数据、命令和口径
- 再定位：需要确认 API、示例或测试边界时，回到对应源码、测试和 benchmark 文件
- Markdown 只记录已经落地并验证过的 C API 资产，不提前补未完成页面；新增 wrapper 的最新事实以源码、测试、示例和 benchmark 为准

## 建议阅读顺序

1. `docs/c/modules/kernel/05-性能测试.md`
2. `src/c/galay-kernel-c/async-c/tcp_socket_c.h`
3. `src/c/galay-kernel-c/async-c/udp_socket_c.h`
4. `src/c/galay-kernel-c/async-c/async_file_c.h`
5. `src/c/galay-kernel-c/async-c/aio_file_c.h`
6. `src/c/galay-kernel-c/async-c/file_watcher_c.h`
7. `src/c/galay-kernel-c/concurrency-c/async_mutex_c.h`
8. `src/c/galay-kernel-c/concurrency-c/async_waiter_c.h`
9. `src/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h`
10. `src/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h`
11. `examples/c/kernel/`
12. `test/c/kernel/`
13. `benchmark/c/kernel/`

## 按任务进入

- 想看当前压测数据：`docs/c/modules/kernel/05-性能测试.md`
- 想复现 benchmark：`docs/c/modules/kernel/05-性能测试.md` 的“推荐复现命令”
- 想照着 C API 写 echo：`examples/c/kernel/e2_tcp_socket_echo.c`
- 想确认 `accept` / `recv` / `send` / `close` callback 结果：`test/c/kernel/t6_tcp_async_callbacks.c`
- 想确认 close 集成路径：`test/c/kernel/t7_tcp_close_integration.c`
- 想看 UDP C ABI：`src/c/galay-kernel-c/async-c/udp_socket_c.h`、`examples/c/kernel/e3_udp_socket_echo.c`、`test/c/kernel/t10_udp_socket_callbacks.c`
- 想看文件 IO C ABI：`src/c/galay-kernel-c/async-c/async_file_c.h`、`src/c/galay-kernel-c/async-c/aio_file_c.h`、`test/c/kernel/t11_async_file_io.c`、`test/c/kernel/t12_aio_file_batch.c`
- 想看文件监控 C ABI：`src/c/galay-kernel-c/async-c/file_watcher_c.h`、`test/c/kernel/t13_file_watcher_events.c`
- 想看 concurrency C ABI：`src/c/galay-kernel-c/concurrency-c/`、`test/c/kernel/t14_async_mutex.c` 到 `test/c/kernel/t17_unsafe_channel.c`

## 按关键词进入

- `galay_kernel_tcp_socket_accept`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`test/c/kernel/t6_tcp_async_callbacks.c`
- `galay_kernel_tcp_socket_recv`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`examples/c/kernel/e2_tcp_socket_echo.c`
- `galay_kernel_tcp_socket_send`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`examples/c/kernel/e2_tcp_socket_echo.c`
- `galay_kernel_tcp_socket_accept_loop`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`test/c/kernel/t8_tcp_loop_callbacks.c`
- `galay_kernel_tcp_socket_recv_loop`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`test/c/kernel/t8_tcp_loop_callbacks.c`
- `galay_kernel_tcp_socket_send_loop`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`test/c/kernel/t8_tcp_loop_callbacks.c`
- `galay_kernel_tcp_socket_close`：`src/c/galay-kernel-c/async-c/tcp_socket_c.h`、`test/c/kernel/t7_tcp_close_integration.c`
- `benchmark_c_kernel_tcp_socket_lifecycle`：`benchmark/c/kernel/b2_tcp_socket_lifecycle.c`、`docs/c/modules/kernel/05-性能测试.md`
- `benchmark_c_kernel_tcp_socket_server_throughput`：`benchmark/c/kernel/b3_tcp_socket_server_throughput.c`、`docs/c/modules/kernel/05-性能测试.md`
- `benchmark_c_kernel_tcp_socket_client_throughput`：`benchmark/c/kernel/b4_tcp_socket_client_throughput.c`、`docs/c/modules/kernel/05-性能测试.md`
- `galay_kernel_udp_socket_recvfrom` / `galay_kernel_udp_socket_sendto`：`src/c/galay-kernel-c/async-c/udp_socket_c.h`、`test/c/kernel/t10_udp_socket_callbacks.c`
- `galay_kernel_async_file_read` / `galay_kernel_async_file_write`：`src/c/galay-kernel-c/async-c/async_file_c.h`、`test/c/kernel/t11_async_file_io.c`
- `galay_kernel_aio_file_commit`：`src/c/galay-kernel-c/async-c/aio_file_c.h`、`test/c/kernel/t12_aio_file_batch.c`
- `galay_kernel_file_watcher_watch`：`src/c/galay-kernel-c/async-c/file_watcher_c.h`、`test/c/kernel/t13_file_watcher_events.c`
- `galay_kernel_async_mutex_lock`：`src/c/galay-kernel-c/concurrency-c/async_mutex_c.h`、`test/c/kernel/t14_async_mutex.c`
- `galay_kernel_async_waiter_wait`：`src/c/galay-kernel-c/concurrency-c/async_waiter_c.h`、`test/c/kernel/t15_async_waiter.c`
- `galay_kernel_mpsc_channel_recv`：`src/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h`、`test/c/kernel/t16_mpsc_channel.c`
- `galay_kernel_unsafe_channel_recv`：`src/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h`、`test/c/kernel/t17_unsafe_channel.c`
