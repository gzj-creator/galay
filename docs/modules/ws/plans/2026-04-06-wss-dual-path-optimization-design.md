# WSS Dual-Path Optimization Design

**Date:** 2026-04-06

**Goal**

在不引入功能回归的前提下，同一轮同时优化 `galay-http` 的 WSS server 和 WSS client 热路径，使：

- `B7-WssServer` 在 Go client 压测下追平或超过 Rust WSS server 对照
- `B8-WssClient` 在 Rust WSS server 和 `galay-http B7-WssServer` 两条基线下都给出清晰结论，并消除当前稳态吞吐损失

**Current Facts**

- 系统安装版 `galay-kernel` 已更新为 `3.4.4`
- 系统安装版 `galay-ssl` 已更新为 `1.2.1`
- `galay-http` 使用系统安装版重新验收时 `ctest 45/45` 通过
- WSS 功能性 bug 已清零：
  - `B8-WssClient 8 clients / 4s / 256B` 稳定，`Failed=0`
  - `B7-WssServer` 在 Go client 下 `20/4/256` 与 `60/8/1024` 均 `fail=0`
- 当前剩余主问题是性能：
  - `B7-WssServer` `60/8/1024` 约 `187k rps`
  - Rust 对照约 `195k rps`
  - 差距约 `4%`

**Scope**

本轮优化只在 `galay-http` 仓库内完成，优先修改：

- `benchmark/b7_wss.cc`
- `benchmark/b8_wss.cc`
- `galay-http/kernel/websocket/ws_reader.h`
- `galay-http/kernel/websocket/ws_writer.h`
- `galay-http/kernel/websocket/ws_conn.h`
- `galay-http/client/websocket/ws_client.h`

不在本轮主动修改：

- `galay-kernel`
- `galay-ssl`
- 与 WSS 无关的 HTTP/1.x、HTTP/2 逻辑

只有在 profile 证据明确显示瓶颈落在系统安装版 `galay-ssl` 接口边界时，才把它记录为下一轮输入，不在本轮直接扩仓修改。

**Acceptance**

功能验收：

- `ctest` 继续保持 `45/45` 通过
- `B8-WssClient 8/4/256` 不崩溃
- `B7-WssServer` 在 Go client 下 `20/4/256` 与 `60/8/1024` 保持 `fail=0`

性能验收：

- `B7-WssServer` 对 Go client 的 `60/8/1024` 不低于 Rust server 对照
- `B8-WssClient` 在两条基线下都重新测量：
  - 对 Rust WSS server
  - 对 `galay-http B7-WssServer`
- 记录吞吐、失败数、平均延迟，若未追平需要明确剩余差距和热点证据

**Recommended Approach**

采用“两阶段、双路径、证据驱动”的方法：

1. 固化基线与 profile
2. 优化 WSS server steady-state
3. 优化 WSS client steady-state
4. 回归与对照复测

原因：

- 当前功能已经稳定，性能问题更适合小步 profile 和小步修复
- 双路径一起纳入目标，但不应一开始混改，否则难以分辨 server/client 各自收益
- `galay-http` 的 WebSocket 层已经形成清晰边界，先从 `WsReader/WsWriter/WsConn/WsClient` 的分配、拷贝、循环和 await 次数入手，收益最大，风险最小

**Hotspot Hypotheses**

优先验证这些热点，而不是凭直觉大改：

- `WsReader` 的帧解析和消息拼装存在额外拷贝或小对象重复构造
- `WsWriter` 在文本/二进制回显路径有重复 header/frame 组装开销
- `WsConn` 的 reader/writer 获取与设置解析存在可消除的 steady-state 成本
- `B8-WssClient` 的欢迎消息、回显循环和统计更新本身引入了额外开销
- benchmark 驱动存在不必要的字符串构造、超时包装或日志路径开销

**Out of Scope**

- 新增 API
- 新增跨仓库依赖
- 调整版本/tag
- 重做 benchmark 协议或更换对照实现

**Verification Strategy**

每次优化都按同一顺序执行：

1. 窄范围 benchmark 或 compile-only 验证
2. 相关单测/回归测试
3. `B7-WssServer` 与 `B8-WssClient` 的主口径 benchmark
4. 对 Rust 和 `galay-http` 双基线做客户端复测

**New Session Minimal Context**

新会话只需要知道：

- 系统安装版 `galay-kernel=3.4.4`
- 系统安装版 `galay-ssl=1.2.1`
- `galay-http` 当前 WSS 功能已稳定、测试已全绿
- 这一轮只优化 `galay-http` 的 WSS server/client 热路径
- 主验收口径是：
  - `B7-WssServer + Go client 60/8/1024`
  - `B8-WssClient -> Rust WSS server`
  - `B8-WssClient -> galay-http B7-WssServer`

