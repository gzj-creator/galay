# WSS Dual-Path Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在同一轮同时优化 `galay-http` 的 WSS server 和 WSS client 热路径，保持功能正确，并让主 benchmark 至少追平 Rust 对照。

**Architecture:** 本轮只在 `galay-http` 仓库内操作，先固定基线与 profile，再分离 server/client 热路径逐步优化。所有修改都必须先有基线数据、后有最小改动、最后有复测证据，避免把功能修复与性能调优混在一起。

**Tech Stack:** C++23, CMake, CTest, `galay-http`, system-installed `galay-kernel 3.4.4`, system-installed `galay-ssl 1.2.1`, Go benchmark client, Rust baseline server

---

### Task 1: 固化系统安装版基线

**Files:**
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/docs/plans/2026-04-06-wss-dual-path-optimization.md`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b7_wss.cc`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b8_wss.cc`

**Step 1: 准备干净的系统安装版构建目录**

Run:
```bash
cmake -S /tmp/galay-http-system-source-20260406-copy \
  -B /tmp/galay-http-wss-opt-baseline \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/usr/local \
  -DGALAY_BUILD_SSL=ON \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=ON \
  -DBUILD_EXAMPLES=OFF
```

Expected: configure 成功，依赖来自 `/usr/local`

**Step 2: 构建测试与 benchmark**

Run:
```bash
cmake --build /tmp/galay-http-wss-opt-baseline --parallel
```

Expected: build 成功

**Step 3: 跑完整测试基线**

Run:
```bash
ctest --test-dir /tmp/galay-http-wss-opt-baseline --output-on-failure
```

Expected: `45/45` pass

**Step 4: 跑 server 主口径基线**

Run:
```bash
/tmp/galay-http-go-proto-client -proto wss -addr 127.0.0.1:<port> -path /ws -conns 60 -duration 8 -size 1024
```

Expected: `fail=0`，记录 `success/rps/avg_ms`

**Step 5: 跑 client 双基线**

Run:
```bash
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<rust-port>/ws 60 8 1024
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<galay-port>/ws 60 8 1024
```

Expected: 两条基线都有稳定结果，记录吞吐与延迟

### Task 2: 采样并定位 WSS server 热点

**Files:**
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_reader.h`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_writer.h`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_conn.h`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b7_wss.cc`

**Step 1: 查找 steady-state 回显路径的对象构造和拷贝点**

Run:
```bash
rg -n "getMessage|sendText|sendBinary|getReader|getWriter|std::string|std::move|reserve|append" \
  /Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket \
  /Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b7_wss.cc
```

Expected: 找到稳定循环中的消息解析、帧编码、字符串复用点

**Step 2: 写下单一假设**

Expected: 明确写成一句话，例如“server steady-state 吞吐主要损失在 `WsReader/WsWriter` 的重复 frame/message 组装与临时字符串管理”

**Step 3: 写一个只覆盖该热点的最小回归或 benchmark 辅助验证**

Expected: 新测试或局部 benchmark 能区分优化前后热点行为

**Step 4: 运行红测或当前基线**

Run: 针对新增测试/局部验证的最小命令

Expected: 记录现状，证明测试能观测目标热点

### Task 3: 优化 WSS server steady-state

**Files:**
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_reader.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_writer.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_conn.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b7_wss.cc`
- Test: 选取受影响的 websocket/wss fast-path 测试

**Step 1: 写 failing/observability 测试**

Expected: 测试准确锁住目标行为，例如减少额外 wakeup、避免重复拷贝、固定 frame 组装路径

**Step 2: 运行它，确认能观测当前问题**

Run: 最小 `ctest -R ...`

Expected: 若是行为测试则先失败；若是 compile/perf surface 测试则先记录旧值

**Step 3: 只做一个最小优化**

Expected: 一次只处理一个热点，例如：
- 复用消息缓冲
- 避免重复创建 writer/reader 临时对象
- 避免不必要的 frame header 重组

**Step 4: 跑受影响测试**

Run:
```bash
ctest --test-dir /tmp/galay-http-wss-opt-baseline --output-on-failure -R '^(T55-wss_writer_steady_state|T60-ws_reader_fast_path|T61-ws_writer_common_fast_path|T62-ws_echo_fewer_wakeups)$'
```

Expected: 相关测试全绿

**Step 5: 跑 server 主口径 benchmark**

Run: `B7-WssServer + Go client 60/8/1024`

Expected: `fail=0` 且吞吐提升；若无提升，回到热点分析

### Task 4: 采样并定位 WSS client 热点

**Files:**
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/client/websocket/ws_client.h`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_reader.h`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_writer.h`
- Reference: `/Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b8_wss.cc`

**Step 1: 查客户端循环中的固定开销**

Run:
```bash
rg -n "upgrade|welcome|getMessage|sendText|latency|atomic|std::string|timeout" \
  /Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b8_wss.cc \
  /Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket
```

Expected: 定位欢迎消息路径、回显循环、统计更新、超时包装等固定成本

**Step 2: 写单一假设**

Expected: 例如“client 吞吐主要损失在 benchmark 自身字符串/统计更新与 message receive 路径的额外 steady-state 开销”

**Step 3: 为该热点准备最小验证**

Expected: 新测试或局部复测能够证明该热点可被消除

### Task 5: 优化 WSS client steady-state

**Files:**
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/benchmark/b8_wss.cc`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/client/websocket/ws_client.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_reader.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/websocket/ws_writer.h`

**Step 1: 先写/更新测试**

Expected: 锁住目标行为，不允许以牺牲正确性换吞吐

**Step 2: 实施最小优化**

Expected: 只做一个热点，例如：
- 欢迎消息和 steady-state 回显路径减少重复分配
- benchmark 统计更新去掉不必要的热路径开销
- client session/upgrade 后的对象复用收敛

**Step 3: 运行客户端相关测试**

Run:
```bash
ctest --test-dir /tmp/galay-http-wss-opt-baseline --output-on-failure -R '^(T20-websocket_client|T55-wss_writer_steady_state|T60-ws_reader_fast_path|T61-ws_writer_common_fast_path|T62-ws_echo_fewer_wakeups)$'
```

Expected: 全绿

**Step 4: 跑 client 双基线**

Run:
```bash
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<rust-port>/ws 60 8 1024
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<galay-port>/ws 60 8 1024
```

Expected: 两条基线都提升，且无崩溃/无错误

### Task 6: 全量回归与验收收口

**Files:**
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/docs/05-性能测试.md`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/README.md`

**Step 1: 跑完整测试**

Run:
```bash
ctest --test-dir /tmp/galay-http-wss-opt-baseline --output-on-failure
```

Expected: `45/45` pass

**Step 2: 重跑 server/client 所有主口径**

Run:
```bash
/tmp/galay-http-go-proto-client -proto wss -addr 127.0.0.1:<galay-port> -path /ws -conns 20 -duration 4 -size 256
/tmp/galay-http-go-proto-client -proto wss -addr 127.0.0.1:<galay-port> -path /ws -conns 60 -duration 8 -size 1024
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<rust-port>/ws 8 4 256
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<rust-port>/ws 60 8 1024
/tmp/galay-http-wss-opt-baseline/benchmark/b8_wssient wss://127.0.0.1:<galay-port>/ws 60 8 1024
```

Expected:
- server 压测 `fail=0`
- client benchmark 不崩
- 与 Rust 对照差距收敛到 0 或转正

**Step 3: 更新文档**

Expected:
- `README.md` 简要补充新的 WSS benchmark 结论
- `docs/05-性能测试.md` 记录 workload、基线、优化后结果

**Step 4: 准备提交**

Run:
```bash
git status --short
```

Expected: 只包含本轮相关文件

**Step 5: 提交**

```bash
git add <仅本轮相关文件>
git commit -m "优化 WSS 双端稳态热路径并补齐性能验收"
```

Plan complete and saved to `docs/plans/2026-04-06-wss-dual-path-optimization.md`. Two execution options:

**1. Subagent-Driven (this session)** - I dispatch fresh subagent per task, review between tasks, fast iteration

**2. Parallel Session (separate)** - Open new session with executing-plans, batch execution with checkpoints

**Which approach?**
