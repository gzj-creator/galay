# TLS Write Path Aggregation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 先收敛 `SslSocket` 场景下 `HttpWriter/WsWriter` 的发送聚合固定成本，验证是否能缩小 `https/wss/h2` 与 Rust 对照的差距。

**Architecture:** 本轮只做 TLS 写路径，不碰 `galay-kernel` 和 `galay-ssl` 本体。先用单元测试锁住 `HttpWriter<SslSocket>` 的 steady-state 聚合行为，再以最小改动把 `response.toString()/request.toString()` 替换成复用缓冲区内直接拼装。

**Tech Stack:** C++23, CMake, CTest, galay-http, galay-ssl

---

### Task 1: 锁住 SSL HttpWriter steady-state 聚合行为

**Files:**
- Create: `/Users/gongzhijie/Desktop/projects/git/galay-http/test/t64_httpswrite.cc`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/kernel/http/http_writer.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/protoc/http/http_response.h`
- Modify: `/Users/gongzhijie/Desktop/projects/git/galay-http/galay-http/protoc/http/http_response.cc`

**Step 1:** 写 failing test，要求 `HttpWriter<SslSocket>` 在 `sendResponse/sendRequest` 后产生正确字节布局，并暴露一次 SSL 聚合发送命中。

**Step 2:** 运行 `ctest --test-dir /tmp/galay-http-wss-opt-current --output-on-failure -R '^T64-https_writer_steady_state$'` 或最小构建命令，确认当前失败。

**Step 3:** 以最小实现增加 SSL 直拼装缓冲区路径，避免 `response.toString()/request.toString()` 的临时整包字符串。

**Step 4:** 跑 `T55/T64` 与相关 `T20` 回归。

**Step 5:** 如果测试和局部 benchmark 都成立，再进入 `B14/B7/B12` 的主口径复测。
