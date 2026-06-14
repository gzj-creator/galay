# HTTP/1.1 Fixed Response Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 HTTP/1.1 固定 `200 OK` benchmark 路径加入预构造响应发送 fast-path，减少每请求构造、序列化和复制成本。

**Architecture:** 在 `HttpWriter` 增加外部连续字节视图发送接口；`B1-HttpServer` 与 `B14-HttpsServer` 改为复用静态响应字节串。现有通用 `sendResponse()` 保持不变，先通过 benchmark 验证这一最小优化的收益。

**Tech Stack:** C++20, CMake, galay-http benchmark harness

---

### Task 1: Add writer view-send fast path

**Files:**
- Modify: `galay-http/galay-http/kernel/http/http_writer.h`

**Step 1: Add a non-owning send interface**

为 `HttpWriterImpl` 增加 `sendView(std::string_view)`，要求调用方保证底层存储在 await 完成前有效。

**Step 2: Wire the send state to support external storage**

让 `bufferData()` / `sentBytes()` / 清理逻辑同时支持内部 `std::string` 和外部只读视图。

**Step 3: Build target to verify compile**

Run: `cmake --build /tmp/galay-http-wss-opt-current --target B1-HttpServer B14-HttpsServer -j8`

Expected: build succeeds

### Task 2: Reuse fixed HTTP/HTTPS responses in benchmarks

**Files:**
- Modify: `benchmark/b1_http.cc`
- Modify: `benchmark/b14_https.cc`

**Step 1: Replace per-request builder path**

把固定 `200 OK` 响应替换为静态字节串，并在循环内使用 `writer.sendView(...)`。

**Step 2: Reuse reader/writer objects inside the connection loop**

将 `reader` / `writer` 提升到循环外，减少 steady-state 小对象反复构造。

**Step 3: Rebuild benchmarks**

Run: `cmake --build /tmp/galay-http-wss-opt-current --target B1-HttpServer B14-HttpsServer -j8`

Expected: build succeeds

### Task 3: Verify performance signal

**Files:**
- No code changes required

**Step 1: Run targeted HTTP benchmark**

Run the existing `B1-HttpServer` benchmark and compare against the previous baseline.

**Step 2: Run targeted HTTPS benchmark**

Run the existing `B14-HttpsServer` benchmark and verify it does not regress.

**Step 3: Commit only if the signal is stable**

如果 HTTP/HTTPS 至少有一条主口径呈现稳定收益，再准备中文 commit；否则保留为实验并继续下一轮分析。
