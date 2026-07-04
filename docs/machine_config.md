# 测试机器配置

## 硬件配置

### CPU
- 型号: Apple M4 Pro
- 核心数: 12
- 线程数: 12
- 架构: arm64
- 性能核心: 8
- 能效核心: 4
- 缓存: Performance L1I 192KB / L1D 128KB / L2 16MB; Efficiency L1I 128KB / L1D 64KB / L2 4MB

### 内存
- 总容量: 48 GiB
- 类型: Apple Silicon 统一内存

### 存储
- 类型: Apple 内置 SSD
- 根分区容量: 460 GiB
- 根分区可用: 7.2 GiB

### 网络
- 回环接口: lo0
- 回环 MTU: 16384
- 测试地址: 127.0.0.1 / ::1

## 软件配置

### 操作系统
- 系统: macOS 26.3.1 (a)
- Build: 25D771280a
- 内核: Darwin 25.3.0
- 架构: arm64

### 编译器与构建工具
- CMake: 4.0.2
- C++ 编译器: Apple clang 17.0.0 (`/usr/bin/c++`)
- C 编译器: Apple clang 17.0.0 (`/usr/bin/cc`)
- OpenSSL: 3.6.2
- Homebrew: 6.0.2

### galay 版本
- Git 版本: `v4.0.1-2-gfd86efe-dirty`
- Git commit: `fd86efe`
- 工作区状态: dirty

## 系统配置

### 资源限制
```bash
ulimit -n # 1048576
ulimit -u # 8000
```

### TCP 参数
- 当前测试机为 macOS，Linux `net.core.somaxconn`、`net.ipv4.tcp_max_syn_backlog`、`net.ipv4.tcp_tw_reuse` 等参数不适用。

### CPU 调度
- Governor: macOS 自动调度，不提供 Linux `performance` governor
- Turbo Boost: Apple Silicon 自动管理
- CPU Affinity: 本轮未绑核

### 系统负载
- 记录时间: 2026-07-03 23:44:10 CST
- 进程采样: 最高后台 CPU 约 2.8%，未进行独占压测隔离

## 构建配置

### 当前 `build/` 目录
```bash
CMAKE_BUILD_TYPE=Debug
GALAY_BUILD_BENCHMARKS=OFF
BUILD_TESTING=OFF
CMAKE_CXX_COMPILER=/usr/bin/c++
CMAKE_C_COMPILER=/usr/bin/cc
```

当前 `build/` 目录不满足 `docs/benchmark_plan.md` 要求的 Release 压测条件；以该目录执行的模块探测若标记为 blocked/unavailable，原因以该构建配置为准。

### 已存在的 `build-release/` 目录
```bash
CMAKE_BUILD_TYPE=Release
GALAY_BUILD_BENCHMARKS=ON
BUILD_TESTING=ON
GALAY_BUILD_C_API=OFF
```

`build-release/` 已包含完整 C++ benchmark 目标清单，但本轮探测时大部分 benchmark 可执行文件尚未实际生成；只有已构建并可运行的二进制才能作为正式压测输入。C API benchmark 不在该构建中，因为 `GALAY_BUILD_C_API=OFF`。

### 正式压测建议构建命令
```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGALAY_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON

cmake --build build-release --parallel
```

## 已探测工具

| 工具 | 状态 | 路径 |
|------|------|------|
| wrk | available | `/opt/homebrew/bin/wrk` |
| h2load | available | `/opt/homebrew/bin/h2load` |
| nghttpd | available | `/opt/homebrew/bin/nghttpd` |
| httpd | available | `/usr/sbin/httpd` |
| redis-server | available | `/opt/homebrew/bin/redis-server` |
| redis-benchmark | available | `/opt/homebrew/bin/redis-benchmark` |
| mysql | available | `/opt/homebrew/bin/mysql` |
| mysqlslap | available | `/opt/homebrew/bin/mysqlslap` |
| etcd | available | `/opt/homebrew/bin/etcd` |
| etcdctl | available | `/opt/homebrew/bin/etcdctl` |
| openssl | available | `/opt/homebrew/bin/openssl` |
| cargo | available | `/Users/gongzhijie/.cargo/bin/cargo` |
| go | available | `/opt/homebrew/bin/go` |
| node | available | `/Users/gongzhijie/.volta/bin/node` |
| npm | available | `/Users/gongzhijie/.volta/bin/npm` |
| h2o | unavailable | 未在 `PATH` 中发现 |
| nginx | unavailable | 未在 `PATH` 中发现 |
| caddy | unavailable | 未在 `PATH` 中发现 |
| websocat | unavailable | 未在 `PATH` 中发现 |
| websocketd | unavailable | 未在 `PATH` 中发现 |
| mongod | unavailable | 未在 `PATH` 中发现 |
| mongosh | unavailable | 未在 `PATH` 中发现 |
| ghz | unavailable | 未在 `PATH` 中发现 |

## 验证信息
- 测试日期: 2026-07-03
- 测试人员: Codex
- 测试轮次: 环境探测与可复现压测准备
- 数据可重现性: 正式 Release 压测尚未完成；模块报告中的 blocked/unavailable 记录可按补跑命令复现。
