# kernel C 文档

## 当前基线

- 公开头文件、实现、示例、测试和 benchmark 依次作为事实来源。
- kernel runtime、stackful coroutine、reactor、TCP/UDP、文件 I/O 和文件监控均为原生 C 实现，不依赖 C++ bridge。
- runtime 公开句柄类型为 `galay_c_runtime_t`；runtime 函数统一使用 `galay_c_runtime_*` 前缀。
- MPMC/MPSC/SPSC 当前提供原生 bounded channel；已删除旧 bridge 和 unbounded C wrapper。

## 建议阅读顺序

1. `src/c/galay-kernel-c/kernel.h` umbrella header
2. `src/c/galay-kernel-c/core-c/runtime.h`
3. `src/c/galay-kernel-c/coro-c/coro_task.h`
4. `src/c/galay-kernel-c/coro-c/coro_wait.h`
5. `src/c/galay-kernel-c/coro-c/coro_sleep.h`
6. `src/c/galay-kernel-c/async-c/tcp_socket.h`
7. `src/c/galay-kernel-c/async-c/udp_socket.h`
8. `src/c/galay-kernel-c/async-c/async_file.h`
9. `src/c/galay-kernel-c/async-c/aio_file.h`
10. `src/c/galay-kernel-c/async-c/file_watcher.h`
11. `src/c/galay-kernel-c/async-c/async_mutex.h`
12. `src/c/galay-kernel-c/async-c/async_waiter.h`
13. `src/c/galay-kernel-c/concurrency-c/{mpmc,mpsc,spsc}/bounded_channel.h`
14. `examples/c/kernel/`
15. `test/c/kernel/`
16. `benchmark/c/kernel/`

## 按任务进入

- TCP echo：`examples/c/kernel/e2_tcp_socket_echo.c`、`test/c/kernel/t25_coro_tcp.c`
- UDP I/O：`examples/c/kernel/e3_udp_socket_echo.c`、`test/c/kernel/t10_udp_socket_callbacks.c`
- coroutine sleep：`examples/c/kernel/e13_coro_sleep.c`、`test/c/kernel/t26_coro_sleep.c`
- 文件 I/O：`test/c/kernel/t11_async_file_io.c`、`test/c/kernel/t12_aio_file_batch.c`
- 文件监控：`examples/c/kernel/e6_file_watcher.c`、`test/c/kernel/t13_file_watcher_events.c`
- timeout：`test/c/kernel/t18_tcp_timeout_callbacks.c` 至 `t21_file_watcher_timeout_callbacks.c`
- bounded channel：`test/c/kernel/t31_native_bounded_channels.c`、`benchmark/c/kernel/b28_mpmc_bounded_channel_throughput.c`、`b30_mpsc_bounded_channel_throughput.c`、`b32_spsc_bounded_channel_throughput.c`
- 性能复现：`docs/c/modules/kernel/05-性能测试.md`

## 关键入口

- TCP：`galay_c_tcp_socket_create/bind/listen/accept/connect/recv/send/readv/writev/sendfile/close`
- UDP：`galay_c_udp_socket_create/bind/recvfrom/sendto/close`
- Async file：`galay_c_async_file_open/read/write/seek/tell/close`
- AIO file：`galay_c_aio_file_open/read/write/fsync/seek/close`
- File watcher：`galay_c_file_watcher_create/add_watch/remove_watch/wait/close`
- Coroutine：`galay_c_coro_spawn/yield/join/cancel/destroy`、`galay_c_coro_sleep`
- Runtime：`galay_c_runtime_create/start/stop/is_running/destroy`
- Bounded channel：`galay_c_{mpmc,mpsc,spsc}_bounded_channel_create/try_send/try_recv/send/recv/close/destroy`

所有非 `void` 返回值都必须检查或显式向上传播。I/O API 返回 `C_IOResult`，调用方先检查 `code`，再读取 `sys_errno`、`bytes`、`value` 或 `ptr`。Channel 的 `C_IOResultClosed` 表示关闭后不再接受新消息，或已关闭队列在 drain 完成后无数据可读。
