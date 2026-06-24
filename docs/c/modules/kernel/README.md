# 文档导航

## 当前基线

- 真相优先级：公开头文件 > 实现 > `examples/c` > `test/c` > `benchmark/c` > Markdown
- 当前 C 文档按 C++ 文档的模块路径和编号口径组织，落点为 `docs/c/modules/kernel`
- 当前已验证环境：macOS / AppleClang 17 / Release 构建 / kqueue
- 当前已落地页面：`docs/c/modules/kernel/05-性能测试.md`
- 当前验证范围：C Kernel `TcpSocket` callback API、`close` 集成路径、echo 示例、QPS/吞吐压测和生命周期压测

## 两层规则

- 先回答：优先在 `docs/c/modules/kernel/05-性能测试.md` 内给出当前可复现的 benchmark 数据、命令和口径
- 再定位：需要确认 API、示例或测试边界时，回到对应源码、测试和 benchmark 文件
- Markdown 只记录已经落地并验证过的 C API 资产，不提前补未完成页面

## 建议阅读顺序

1. `docs/c/modules/kernel/05-性能测试.md`
2. `src/c/galay-kernel-c/async-c/tcp_socket_c.h`
3. `examples/c/kernel/e2_tcp_socket_echo.c`
4. `test/c/kernel/t6_tcp_async_callbacks.c`
5. `test/c/kernel/t7_tcp_close_integration.c`
6. `test/c/kernel/t8_tcp_loop_callbacks.c`
7. `benchmark/c/kernel/b2_tcp_socket_lifecycle.c`
8. `benchmark/c/kernel/b3_tcp_socket_server_throughput.c`
9. `benchmark/c/kernel/b4_tcp_socket_client_throughput.c`

## 按任务进入

- 想看当前压测数据：`docs/c/modules/kernel/05-性能测试.md`
- 想复现 benchmark：`docs/c/modules/kernel/05-性能测试.md` 的“推荐复现命令”
- 想照着 C API 写 echo：`examples/c/kernel/e2_tcp_socket_echo.c`
- 想确认 `accept` / `recv` / `send` / `close` callback 结果：`test/c/kernel/t6_tcp_async_callbacks.c`
- 想确认 close 集成路径：`test/c/kernel/t7_tcp_close_integration.c`

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
