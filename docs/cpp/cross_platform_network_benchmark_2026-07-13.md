# Galay 跨平台 C/C++ 网络竞品性能对比（2026-07-13）

## 1. 结论摘要

本次在当前代码 `add162959bd0bbeff6b6d5f7bfc7762a4bdb3a6a` 上，对 TCP、UDP、HTTP 固定响应、HTTP/1.1 静态文件、WebSocket、TLS 1.3 新握手和 HTTP/2 POST echo 进行了 loopback 微基准。竞品全部为 C/C++ 实现，每组轮换执行顺序，并取三次有效样本的中位数。

> **2026-07-14 UDP 补充结论：** multishot recvmsg 优化后，按 5 秒 warmup、30 秒、绑核和轮换顺序重跑，
> Galay io_uring UDP 中位 `138,241 pkt/s`，libuv 1.48.0 中位 `139,712 pkt/s`，差异 **-1.05%**，
> 小于 Galay 自身 CV，视为基本持平。下列 `-13.21%` 保留为 2026-07-13 短时历史结果，不再代表优化后状态。

- Linux 同机直接对比中，io_uring 相对 epoll 在 TCP、HTTP、HTTP/1.1、WebSocket、HTTP/2 上分别提高 **7.70%、48.28%、15.08%、28.49%、16.20%**；TLS 仅提高 0.39%，可视为持平；UDP 下降 17.52%。
- macOS / kqueue 上，Galay 的 HTTP 固定响应比 libevent 高 37.28%，HTTP/1.1 静态文件比 Apache 高 12.09%；WebSocket 和 TLS 分别比 libwebsockets、OpenSSL 低 1.78% 和 3.65%，基本接近；TCP、UDP 分别低 32.62% 和 16.92%。
- Linux / epoll 上，Galay 的 HTTP、HTTP/1.1、TLS 分别比对应竞品高 30.12%、115.46%、11.37%；TCP 低 2.98%，WebSocket 低 17.53%。
- Linux / io_uring 上，Galay 的 TCP、HTTP、HTTP/1.1、WebSocket、TLS 分别比对应竞品高 4.49%、92.93%、147.96%、5.97%、11.80%；UDP 低 13.21%。
- HTTP/2 的 nghttpd 对照不进入正式框架排名：Linux 三轮均有大量失败请求；macOS 虽然请求成功，但三轮吞吐相差 4.1 倍，且 `nghttpd --echo-upload` 与 Galay 框架级 echo 路径不完全同构。

这些结论只适用于本次单机 loopback、短时、高并发微基准。macOS M4 Pro 与 Linux Xeon 是不同硬件，**不得用两台机器的绝对值做跨平台排名**；只有 Linux epoll 与 io_uring 是同一台机器、同一份源码、同一客户端口径下的直接后端对比。

## 2. 对照对象与依赖

| 场景 | Galay 后端 | C/C++ 竞品 | 竞品版本 | 对照内容 |
|---|---|---|---|---|
| TCP echo | kqueue / epoll / io_uring | libuv | macOS 1.51.0；Linux 1.48.0 | 单事件循环 TCP echo server |
| UDP echo | kqueue / epoll / io_uring | libuv | macOS 1.51.0；Linux 1.48.0 | 单事件循环 UDP echo server |
| HTTP 固定响应 | kqueue / epoll / io_uring | libevent evhttp | 2.1.12-stable | 固定 2B HTTP 响应 |
| HTTP/1.1 静态文件 | kqueue / epoll / io_uring | Apache httpd event MPM | macOS 2.4.62；Linux 2.4.58 | 1KB 静态文件、keep-alive |
| WebSocket echo | kqueue / epoll / io_uring | libwebsockets | 4.5.8 | 1024B binary echo |
| TLS 新握手 | kqueue / epoll / io_uring | OpenSSL `s_server` | macOS 3.6.2；Linux 3.0.13 | TLS 1.3 full handshake |
| HTTP/2 POST echo | kqueue / epoll / io_uring | nghttp2 `nghttpd` | macOS 1.69.0；Linux 1.59.0 | h2c、1KB POST、100 streams/connection |

仓库已包含本次使用的 `thirdparty/libwebsockets-4.5.8.tar.gz`，SHA-256 为：

```text
b6ade658f4af3a823d0dc806ae5ef0623f0f4f5e2aeb895a0f77c4783840c30e
```

libwebsockets 使用 Release 静态构建，关闭 SSL、client 和 extensions，只保留本次明文 echo server 所需功能。其余依赖来自系统或包管理器，没有额外源码需要放入 `thirdparty/`。

复现环境通常需要以下包：

```text
macOS/Homebrew: libuv libevent openssl@3 nghttp2 wrk
Ubuntu: build-essential cmake ninja-build pkg-config liburing-dev libuv1-dev
        libevent-dev apache2 libssl-dev nghttp2-client nghttp2-server wrk
```

macOS 使用系统 `/usr/sbin/httpd`；Linux io_uring 还要求内核和 liburing 可用。本次 Linux 服务端日志已明确输出 `Using IOUringScheduler (Linux io_uring)`，不是仅凭编译选项推断后端。

## 3. 测试环境

| 项目 | macOS / kqueue | Linux / epoll 与 io_uring |
|---|---|---|
| 操作系统 | macOS 26.3.1，Darwin 25.3.0 | Ubuntu，Linux 6.8.0-101-generic |
| CPU | Apple M4 Pro，12 核（8P+4E） | Intel Xeon Platinum 8255C 2.50GHz，4 vCPU |
| 内存 | 48GB | 3.9GB |
| 架构 | arm64 | x86_64 |
| 编译器 | AppleClang 17.0.0 | GCC/G++ 14.2.0 |
| 构建类型 | Release | Release |
| io_uring | 不适用 | liburing 2.5 |

本次未固定 CPU 核、未隔离系统噪声，也未同步采集 CPU 和 RSS，因此报告只比较同机同口径下的吞吐和客户端延迟，不给出资源效率结论。

## 4. 统一测试口径

| 场景 | 负载 | 主指标 | 有效性门禁 |
|---|---|---|---|
| TCP | 32 连接、1024B echo、5s | req/s | `Errors=0` 且连接失败为 0 |
| UDP | 100 客户端、256B echo、5s | 接收 pkt/s | 收到有效数据；同时披露丢包率 |
| HTTP | 固定 2B 响应；`wrk -t2 -c64 -d5s --latency` | req/s | socket errors 和 non-2xx 均为 0 |
| HTTP/1.1 | 1KB 静态文件；`wrk -t2 -c64 -d5s --latency` | req/s | socket errors 和 non-2xx 均为 0 |
| WebSocket | 100 连接、1024B echo、5s | msg/s | failed messages 为 0 |
| TLS | TLS 1.3、`TLS_AES_128_GCM_SHA256`、禁用 ticket、连续新握手 | wall conn/s | OpenSSL 输出正数 real-time connection statistics |
| HTTP/2 | h2c、1KB POST echo；`h2load -t2 -c20 -m100`，warmup 1s + 5s | req/s | failed、errored、timeout 均为 0 |

每组按轮次旋转 Galay 后端和竞品的运行顺序，取三次有效样本的中位数。Linux 的 TCP、UDP、WebSocket 统一使用 epoll 构建的 Galay 客户端发压，以隔离服务端 epoll/io_uring 差异。所有服务端和客户端均运行在同一台机器，通过 `127.0.0.1` 通信。

HTTP/1.1 中 Galay 静态文件 server 使用参数 `4`；Apache 使用 event MPM：

```apache
StartServers 4
ServerLimit 4
ThreadsPerChild 64
MaxRequestWorkers 256
MaxConnectionsPerChild 0
```

旧的 `MaxRequestWorkers=64` 配置会触发 `AH10159` 并产生 wrk read errors，旧结果已全部排除。

## 5. macOS / kqueue 结果

### 5.1 吞吐中位数

| 场景 | Galay kqueue | 竞品 | Galay 相对竞品 | 备注 |
|---|---:|---:|---:|---|
| TCP req/s | 124,096 | libuv 184,170 | -32.62% | Galay 三轮波动明显 |
| UDP pkt/s | 116,438 | libuv 140,158 | -16.92% | 中位丢包率 0.799% vs 0.363% |
| HTTP req/s | 190,487.91 | libevent 138,755.19 | +37.28% | 全部零错误 |
| HTTP/1.1 req/s | 59,621.37 | Apache 53,188.33 | +12.09% | 修正 event MPM 配置后全部零错误 |
| WebSocket msg/s | 160,136 | libwebsockets 163,037 | -1.78% | 平均延迟 0.623ms vs 0.612ms |
| TLS wall conn/s | 1,099.83 | OpenSSL 1,141.50 | -3.65% | 短时新握手测试 |
| HTTP/2 req/s | 962,648.8 | nghttpd 12,017.6 | 不排名 | nghttpd 三轮 15,767.0 / 12,017.6 / 3,845.6 |

### 5.2 HTTP 延迟中位数

| 场景 / 实现 | p50 | p99 |
|---|---:|---:|
| HTTP / Galay kqueue | 0.295ms | 0.706ms |
| HTTP / libevent | 0.487ms | 0.890ms |
| HTTP/1.1 / Galay kqueue | 1.070ms | 2.160ms |
| HTTP/1.1 / Apache | 0.940ms | 25.180ms |

### 5.3 三轮原始主指标

| 场景 / 实现 | run1 | run2 | run3 | 有效样本 |
|---|---:|---:|---:|---:|
| TCP / Galay kqueue | 175,291 | 114,471 | 124,096 | 3/3 |
| TCP / libuv | 184,170 | 185,817 | 177,274 | 3/3 |
| UDP / Galay kqueue | 116,438 | 102,562 | 121,705 | 3/3 |
| UDP / libuv | 138,114 | 140,158 | 143,224 | 3/3 |
| HTTP / Galay kqueue | 191,990.93 | 190,487.91 | 189,348.40 | 3/3 |
| HTTP / libevent | 119,249.78 | 138,755.19 | 142,311.23 | 3/3 |
| HTTP/1.1 / Galay kqueue | 58,413.01 | 59,978.67 | 59,621.37 | 3/3 |
| HTTP/1.1 / Apache | 53,280.68 | 53,117.79 | 53,188.33 | 3/3 |
| WebSocket / Galay kqueue | 141,874 | 160,726 | 160,136 | 3/3 |
| WebSocket / libwebsockets | 155,423 | 163,037 | 164,439 | 3/3 |
| TLS / Galay kqueue | 1,217.00 | 1,099.83 | 1,050.17 | 3/3 |
| TLS / OpenSSL | 1,141.50 | 1,097.67 | 1,205.17 | 3/3 |
| HTTP/2 / Galay kqueue | 974,623.6 | 962,648.8 | 885,632.0 | 3/3 |
| HTTP/2 / nghttpd | 15,767.0 | 12,017.6 | 3,845.6 | 3/3，仅参考 |

TCP 的最大值是最小值的 1.53 倍，WebSocket、TLS 也有可见的首轮偏高现象。HTTP 固定响应和最终 HTTP/1.1 样本更稳定，因此 macOS TCP、UDP、WebSocket、TLS 的小比例差异需要谨慎解释。

## 6. Linux / epoll、io_uring 结果

### 6.1 吞吐中位数

| 场景 | Galay epoll | Galay io_uring | 竞品 | epoll 相对竞品 | io_uring 相对竞品 |
|---|---:|---:|---:|---:|---:|
| TCP req/s | 68,735 | 74,027 | libuv 70,844 | -2.98% | +4.49% |
| UDP pkt/s | 138,892 | 114,559 | libuv 131,998 | +5.22% | -13.21% |
| HTTP req/s | 50,386.71 | 74,711.01 | libevent 38,724.08 | +30.12% | +92.93% |
| HTTP/1.1 req/s | 56,904.98 | 65,486.58 | Apache 26,410.53 | +115.46% | +147.96% |
| WebSocket msg/s | 53,450.4 | 68,678.5 | libwebsockets 64,812.0 | -17.53% | +5.97% |
| TLS wall conn/s | 383.67 | 385.17 | OpenSSL 344.50 | +11.37% | +11.80% |
| HTTP/2 req/s | 223,124.0 | 259,268.2 | nghttpd 无有效值 | 不排名 | 不排名 |

UDP 中位丢包率分别为 epoll 0.494%、io_uring 0.729%、libuv 0.688%。io_uring 的 UDP 三轮吞吐离散度也高于 epoll，因此当前证据只支持“本负载下 io_uring UDP 未获益”，不足以外推到真实网络或其他批量收发策略。

### 6.2 io_uring 相对 epoll

| 场景 | io_uring 相对 epoll |
|---|---:|
| TCP | +7.70% |
| UDP | -17.52% |
| HTTP | +48.28% |
| HTTP/1.1 | +15.08% |
| WebSocket | +28.49% |
| TLS | +0.39% |
| HTTP/2 | +16.20% |

### 6.3 HTTP 延迟中位数

| 场景 / 实现 | p50 | p99 |
|---|---:|---:|
| HTTP / Galay epoll | 1.180ms | 3.630ms |
| HTTP / Galay io_uring | 0.742ms | 2.870ms |
| HTTP / libevent | 1.510ms | 3.950ms |
| HTTP/1.1 / Galay epoll | 0.860ms | 3.030ms |
| HTTP/1.1 / Galay io_uring | 0.719ms | 2.010ms |
| HTTP/1.1 / Apache | 1.770ms | 8.130ms |

### 6.4 三轮原始主指标

| 场景 / 实现 | run1 | run2 | run3 | 有效样本 |
|---|---:|---:|---:|---:|
| TCP / Galay epoll | 68,901 | 68,735 | 67,676 | 3/3 |
| TCP / Galay io_uring | 74,342 | 74,027 | 73,652 | 3/3 |
| TCP / libuv | 70,844 | 71,049 | 69,849 | 3/3 |
| UDP / Galay epoll | 138,892 | 121,846 | 142,071 | 3/3 |
| UDP / Galay io_uring | 135,696 | 111,002 | 114,559 | 3/3 |
| UDP / libuv | 133,220 | 128,417 | 131,998 | 3/3 |
| HTTP / Galay epoll | 51,809.37 | 50,386.71 | 50,199.58 | 3/3 |
| HTTP / Galay io_uring | 75,225.35 | 74,587.02 | 74,711.01 | 3/3 |
| HTTP / libevent | 38,574.50 | 38,724.08 | 38,972.86 | 3/3 |
| HTTP/1.1 / Galay epoll | 56,682.70 | 57,144.88 | 56,904.98 | 3/3 |
| HTTP/1.1 / Galay io_uring | 65,400.56 | 65,486.58 | 66,893.91 | 3/3 |
| HTTP/1.1 / Apache | 26,410.53 | 26,087.73 | 26,662.40 | 3/4 |
| WebSocket / Galay epoll | 53,450.4 | 53,184.5 | 53,738.7 | 3/3 |
| WebSocket / Galay io_uring | 69,447.3 | 67,380.0 | 68,678.5 | 3/3 |
| WebSocket / libwebsockets | 66,384.5 | 64,812.0 | 64,451.0 | 3/3 |
| TLS / Galay epoll | 424.83 | 383.67 | 382.17 | 3/3 |
| TLS / Galay io_uring | 405.67 | 385.17 | 367.17 | 3/3 |
| TLS / OpenSSL | 369.17 | 344.50 | 342.67 | 3/3 |
| HTTP/2 / Galay epoll | 245,331.8 | 223,124.0 | 222,050.8 | 3/3 |
| HTTP/2 / Galay io_uring | 259,268.2 | 243,033.2 | 261,031.2 | 3/3 |
| HTTP/2 / nghttpd | 25,574.6 | 26,067.6 | 25,555.6 | 0/3，不计入 |

Apache 表中的三项是有效 run1、run3 和补充 run4。原 run2 为 26,314.38 req/s，但出现 `read 8`，已保留原始日志并排除。补样没有覆盖无效结果。

Linux nghttpd 三轮分别出现 72,423、70,572、72,415 个 failed 请求，并被 h2load 同时计入 errored；因此表中的原始吞吐只用于故障定位，不构成有效性能结果。

## 7. 结果解释与限制

1. **不能跨机器比绝对值。** M4 Pro 与 4 vCPU Xeon 的核心、频率、内存和虚拟化环境均不同，macOS 与 Linux 表格只能各自在本机比较。
2. **epoll/io_uring 可以同机比较。** 两者来自同一 Linux 源码提交和测试机，Galay 仅切换 `GALAY_DISABLE_IOURING` 构建选项；TCP、UDP、WebSocket 还使用同一个 epoll 客户端发压。
3. **这是 loopback 微基准。** 没有真实网卡、交换机、RTT、丢包恢复和跨机带宽约束，结果主要反映协议路径、调度器和本机 syscall 开销。
4. **短时测试容易受热机和系统噪声影响。** 轮换顺序和三次中位数降低了偏差，但 macOS TCP、Linux UDP、TLS 和 macOS nghttpd 仍有较大离散度。
5. **线程配置并非“相同线程数”。** 本次比较的是各实现的实际可用服务方式和固定配置，不是单线程算法对决；Apache event MPM 使用 256 workers，Galay 静态文件 server 使用参数 4。
6. **没有资源效率数据。** 未同步记录 CPU、上下文切换、syscall、RSS、能耗或 perf profile，不能仅根据 req/s 判断每核效率。
7. **HTTP/2 竞品证据不足。** Linux nghttpd 无有效样本；macOS nghttpd 波动过大且 echo 路径不完全同构。后续应换成可稳定承载 POST echo、能够校验响应体的 C/C++ HTTP/2 server harness，再做正式排名。

## 8. 后续建议

- 对 Linux UDP 分别采集 epoll/io_uring 的 `perf stat`、批量收发大小、SQ/CQ 深度和丢包位置，确认 io_uring 下降来自提交/完成队列开销、缓冲策略还是客户端压力上限。
- 将时长扩展到 30–60s，增加 warmup，并固定 CPU affinity；同时采集服务端 CPU、RSS、上下文切换和 syscall 数量。
- macOS TCP 需要单独重跑更多轮次，排查首轮显著偏高现象后再发布稳定结论。
- HTTP/2 使用同等线程数、同等响应体校验、同等流控窗口的 C/C++ echo harness，替换当前 nghttpd 参考项。
- 跨机器性能应使用相同型号裸机或同规格虚拟机；真实部署结论应增加跨机 RTT、网卡带宽和并发连接阶梯。

## 9. 证据记录

本次执行期间的脚本、解析结果和原始 stdout 位于：

```text
/tmp/galay-cross-platform-20260713/run-matrix.sh
/tmp/galay-cross-platform-20260713/parse_results.py
/tmp/galay-cross-platform-20260713/summary.json
/tmp/galay-cross-platform-20260713/results/macos
/tmp/galay-cross-platform-20260713/results/macos-http1-final
/tmp/galay-cross-platform-20260713/results/linux
/tmp/galay-cross-platform-20260713/results/linux-http1-final
```

这些 `/tmp` 文件不是仓库永久制品，因此本报告同时保留了所有正式计入样本的主指标、有效样本数、错误排除规则、环境和完整负载参数。发布或长期归档前，可再将原始 stdout 打包存入专用 benchmark artifact。

## 10. 2026-07-14 UDP 优化后竞品补测

原表中的 Linux UDP 为 5 秒短时、未绑核结果。UDP multishot recvmsg 落地后，使用固定 CPU affinity、5 秒 warmup、
30 秒正式测量重新比较 Galay io_uring 与 libuv 1.48.0。两者使用完全相同的 Galay epoll 客户端，服务端/客户端
CPU 分别固定为 `0-1` / `2-3`，并轮换运行顺序。

| 实现 | run1 | run2 | run3 | 中位数 | CV | 丢包率中位数 |
|---|---:|---:|---:|---:|---:|---:|
| Galay io_uring multishot | 135,705 | 138,241 | 145,237 | **138,241** | 2.885% | **0.217%** |
| libuv 1.48.0 / epoll | 141,924 | 139,600 | 139,712 | **139,712** | 0.762% | 0.249% |

Galay 相对 libuv 为 **-1.05%**，差距小于 Galay 自身 CV，按本报告噪声纪律视为基本持平。新结果替代
“优化后仍落后 libuv 13.21%”这一判断，但不覆盖原表的历史原始数据。完整样本位于
`docs/cpp/modules/kernel/benchmark_data/transport_udp_competitor_2026-07-14.csv`。

## 11. 2026-07-14 WebSocket epoll 竞品补测

使用与 UDP 补测相同的固定 CPU、warmup、30 秒正式测量和轮换顺序纪律，重新比较 Galay epoll 与
libwebsockets 4.5.8。两者均由同一个 Galay epoll 客户端发压，服务端均为 1 个 IO thread。

| 实现 | run1 | run2 | run3 | 中位数 | CV | 中位平均 RTT |
|---|---:|---:|---:|---:|---:|---:|
| Galay epoll | 56,173.2 | 52,332.1 | 52,841.4 | **52,841.4 msg/s** | 3.167% | 0.604824 ms |
| libwebsockets 4.5.8 | 75,840.1 | 65,879.4 | 67,929.2 | **67,929.2 msg/s** | 6.146% | 0.470321 ms |

Galay epoll 相对 libwebsockets 为 **-22.21%**。libwebsockets 的 CV 略高于 5%，所以精确差值不能脱离本机
负载复用；但竞品最低轮仍比 Galay 最高轮高 **17.28%**，排名没有样本重叠。Linux/epoll profile 同时表明
每消息 shared state 分配不是热点，因此后续应调查 reactor 唤醒、`readv` / `sendmsg`、`epoll_ctl` 和
sequence 注册切换，而不是实施对象池。完整样本位于
`docs/cpp/modules/ws/benchmark_data/ws_epoll_competitor_2026-07-14.csv`。
