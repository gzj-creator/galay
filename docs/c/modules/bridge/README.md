# bridge C 文档

## 当前基线

- 源码位置：`src/c/galay-bridge-c`
- CMake target：`galay-c-bridge`，alias 为 `galay::c-bridge`
- 主要职责：把 C++ kernel awaitable/controller 细节隔离在 bridge 层，为 C coroutine runtime 和 public C ABI wrapper 提供等待、完成回写与资源释放入口。

## 范围

- TCP/UDP coroutine bridge：`src/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h`、`c_coro_udp_bridge.h`
- 文件 I/O bridge：`c_coro_async_file_bridge.h`、`c_coro_aio_file_bridge.h`
- 文件监控 bridge：`c_coro_file_watcher_bridge.h`
- 并发原语 bridge：`c_coro_async_mutex_bridge.h`、`c_coro_async_waiter_bridge.h`

## 使用约束

- bridge 头文件是 C ABI 内部适配层，不是应用侧首选入口。
- 调用方必须提供完整的 wait ops：`wait`、`complete_user_data`、`release_user_data` 均不能为空。
- direct coroutine I/O 返回 `GalayCoreCoroIOResult`，调用方必须先检查 `code`，再读取 `bytes`、`value` 或 `ptr`。
- `user_data` token 的生命周期由 runtime 提供方负责，必须在完成、取消或清理路径中可被安全回写和释放。

## 建议阅读顺序

1. `src/c/galay-bridge-c/coro-c/c_coro_tcp_bridge.h`
2. `src/c/galay-bridge-c/coro-c/c_coro_async_mutex_bridge.h`
3. `src/c/galay-kernel-c/` 中调用 bridge 的 public C wrapper
4. `test/c/kernel/` 中 direct coroutine 相关测试

