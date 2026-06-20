# HTTP/2 Dispatch Scheduler Production Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 `src/galay-http2/kernel/frame_disp.h` 与 `src/galay-http2/kernel/out_sched.h` 从最小骨架升级为生产可用的 HTTP/2 帧分发、流状态、流控和出站调度核心。

**Architecture:** 保持 `h2_core` 负责连接编排，`frame_disp` 负责协议帧合法性与分发动作，`out_sched` 负责出站控制帧优先级、DATA 流控和公平调度。参考 nghttp2/Envoy/Netty 的分层方式，但不照搬完整 priority dependency tree，先用显式状态机和 Deficit Round Robin 保持代码简洁并保留扩展点。

**Tech Stack:** C++23, galay `Task<T>`, `std::expected`, HTTP/2 frame codec, CMake/CTest, RFC 9113 semantics.

---

## 参考依据

- HTTP/2 规范：RFC 9113，重点参考 stream lifecycle、frame stream id 约束、flow control、GOAWAY、WINDOW_UPDATE。
  - https://datatracker.ietf.org/doc/html/rfc9113
- nghttp2：将 HTTP/2 framing 层做成可复用库，连接层负责调度和回调编排。
  - https://nghttp2.org/documentation/
- Envoy：HTTP/2 codec 基于 nghttp2，把连接字节流解码并复用为独立 HTTP streams。
  - https://www.envoyproxy.io/docs/envoy/latest/intro/life_of_a_request
- Netty：将 HTTP/2 flow controller 抽成独立接口，窗口变化与出站调度分离。
  - https://netty.io/4.1/api/io/netty/handler/codec/http2/Http2FlowController.html

## 设计原则

- 不引入大型框架式抽象；每个类只拥有一个清晰职责。
- 正常协议错误通过 typed result/action 返回，不抛异常，不 `abort/terminate`。
- `Task<T>` 路径不做阻塞等待；调度核心只产出动作，由 `h2_core` 异步写 socket。
- 先补正确性，再做性能优化；测试先行。
- DATA 受 HTTP/2 flow control 限制；控制帧、HEADERS、RST、PING ACK、SETTINGS ACK 不被 DATA 窗口阻塞。
- 初版公平调度用 Deficit Round Robin，保留替换为完整 priority tree 的边界。
- 每完成一个 Task 且该 Task 列出的验证标准满足后，才把标题里的 `[ ]` 改成 `[x]`。Task 1 是 RED 基线，只有新增测试按预期失败且失败原因确认正确后才打勾；Task 2 起必须对应验证命令实际 PASS 后才打勾。不得只因代码写完或局部编译通过就打勾。

## [ ] Task 1: 协议边界测试基线

**Files:**
- Modify: `test/http2/t34_h2disp.cc`
- Modify: `test/http2/t35_h2flow.cc`
- Optional Create: `test/http2/t85_h2dispatch_protocol.cc`
- Optional Create: `test/http2/t86_h2out_scheduler.cc`

**Step 1: 写 RED 测试**

覆盖以下行为：
- DATA/HEADERS/PRIORITY/RST_STREAM/CONTINUATION 的 `stream_id == 0` 返回连接级 `ProtocolError`。
- SETTINGS/PING/GOAWAY 的 `stream_id != 0` 返回连接级 `ProtocolError`。
- CONTINUATION 期待期间收到其他帧返回 GOAWAY。
- WINDOW_UPDATE increment 为 0：stream 0 返回 GOAWAY，非 0 返回 RST_STREAM。
- GOAWAY 后新 stream 被拒绝，已存在 stream 可继续。
- 空 body 且 `end_stream=true` 产生零长度 `DATA END_STREAM`。
- 多个 stream 同时 pending 时，低权重流不会永久饥饿。

**Step 2: 运行测试确认失败**

Run:
```bash
rtk cmake --build build --target t34_h2disp t35_h2flow
rtk ctest --test-dir build -R '^http2\\.(h2disp|h2flow)' --output-on-failure
```

Expected: 新增断言失败，原因是当前 `frame_disp`/`out_sched` 尚未实现这些协议语义。

**Step 3: 不写生产代码**

本任务只建立失败测试。任何生产实现放到后续任务。

## [ ] Task 2: 重塑 frame_disp 结果模型

**Files:**
- Modify: `src/galay-http2/kernel/frame_disp.h`
- Modify: `src/galay-http2/kernel/frame_disp.cc`
- Modify: `test/http2/t34_h2disp.cc`

**Step 1: 写/调整测试**

让测试不只检查 `ok`，还检查错误作用域和动作：
- connection error 生成 `SendGoaway`
- stream error 生成 `SendRstStream`
- 正常帧生成 `DeliverToStream` 或 `Ignore`

**Step 2: 修改结果类型**

建议结构：
```cpp
enum class H2DispatchErrorScope {
    None,
    Connection,
    Stream,
};

enum class H2DispatchActionType {
    DeliverToStream,
    SendGoaway,
    SendRstStream,
    AckSettings,
    AckPing,
    UpdateWindow,
    Ignore,
};

struct H2DispatchAction {
    H2DispatchActionType type = H2DispatchActionType::Ignore;
    uint32_t stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
};

struct H2DispatchResult {
    bool ok = true;
    H2DispatchErrorScope error_scope = H2DispatchErrorScope::None;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
    std::vector<H2DispatchAction> actions;
};
```

**Step 3: 最小实现**

保留现有 CONTINUATION/GOAWAY/WINDOW_UPDATE 行为，只把结果表达迁移到新模型。

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t34_h2disp
rtk ctest --test-dir build -R '^http2\\.h2disp$' --output-on-failure
```

Expected: `http2.h2disp` PASS。

## [ ] Task 3: 补 frame_disp 帧 stream id 约束

**Files:**
- Modify: `src/galay-http2/kernel/frame_disp.cc`
- Modify: `test/http2/t34_h2disp.cc`

**Step 1: 写 RED 测试**

逐类构造非法 stream id：
- DATA stream 0
- HEADERS stream 0
- RST_STREAM stream 0
- PRIORITY stream 0
- CONTINUATION stream 0
- SETTINGS stream 非 0
- PING stream 非 0
- GOAWAY stream 非 0

**Step 2: 实现校验 helper**

在 `.cc` 内部加局部 helper，不暴露新 public API：
```cpp
bool requiresNonZeroStream(const Http2Frame& frame);
bool requiresZeroStream(const Http2Frame& frame);
H2DispatchResult connectionError(Http2ErrorCode code);
H2DispatchResult streamError(uint32_t stream_id, Http2ErrorCode code);
```

**Step 3: 验证**

Run:
```bash
rtk cmake --build build --target t34_h2disp
rtk ctest --test-dir build -R '^http2\\.h2disp$' --output-on-failure
```

Expected: PASS。

## [ ] Task 4: 引入最小 stream lifecycle

**Files:**
- Modify: `src/galay-http2/kernel/frame_disp.h`
- Modify: `src/galay-http2/kernel/frame_disp.cc`
- Modify: `test/http2/t34_h2disp.cc`

**Step 1: 写 RED 测试**

覆盖：
- HEADERS 在 idle stream 上创建 `Open`。
- HEADERS 带 END_STREAM 创建 `HalfClosedRemote`。
- DATA 带 END_STREAM 从 `Open` 到 `HalfClosedRemote`。
- RST_STREAM 进入 `Closed`。
- Closed stream 收 DATA/HEADERS 返回 stream error。

**Step 2: 增加状态**

```cpp
enum class H2StreamLifecycleState {
    Idle,
    ReservedLocal,
    ReservedRemote,
    Open,
    HalfClosedLocal,
    HalfClosedRemote,
    Closed,
};

struct H2DispatcherStreamState {
    H2StreamLifecycleState lifecycle = H2StreamLifecycleState::Idle;
};
```

`H2DispatcherConnectionState` 增加：
```cpp
uint32_t last_peer_stream_id = 0;
uint32_t goaway_last_stream_id = 0;
std::unordered_map<uint32_t, H2DispatcherStreamState> streams;
```

**Step 3: 最小状态迁移**

先只处理 HEADERS/DATA/RST_STREAM/END_STREAM，不实现 PUSH_PROMISE reserved 状态的完整语义。

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t34_h2disp
rtk ctest --test-dir build -R '^http2\\.h2disp$' --output-on-failure
```

Expected: PASS。

## [ ] Task 5: GOAWAY 后的新流拒绝策略

**Files:**
- Modify: `src/galay-http2/kernel/frame_disp.cc`
- Modify: `test/http2/t34_h2disp.cc`

**Step 1: 写 RED 测试**

覆盖：
- 收到 GOAWAY 后记录 `goaway_received` 和 `goaway_last_stream_id`。
- stream id 大于 `goaway_last_stream_id` 的新 HEADERS 被拒绝。
- 已存在 stream 的 DATA/RST_STREAM 仍允许进入分发。

**Step 2: 实现**

利用 `Http2GoAwayFrame::lastStreamId()` 更新 state。只阻止新流，不清理已存在 stream。

**Step 3: 验证**

Run:
```bash
rtk cmake --build build --target t34_h2disp
rtk ctest --test-dir build -R '^http2\\.h2disp$' --output-on-failure
```

Expected: PASS。

## [ ] Task 6: 拆出 flow control 状态

**Files:**
- Create: `src/galay-http2/kernel/flow_control.h`
- Create: `src/galay-http2/kernel/flow_control.cc`
- Modify: `src/galay-http2/CMakeLists.txt` if explicit source list is introduced later; current glob should pick up `.cc`
- Create or Modify: `test/http2/t35_h2flow.cc`

**Step 1: 写 RED 测试**

覆盖：
- connection send window 消耗。
- stream send window 消耗。
- WINDOW_UPDATE 增加窗口。
- SETTINGS_INITIAL_WINDOW_SIZE delta 更新所有 stream window。
- 窗口增量溢出返回 `FlowControlError`。

**Step 2: 定义 API**

```cpp
enum class H2FlowControlError {
    WindowOverflow,
    UnknownStream,
};

struct H2SendWindow {
    int64_t conn_window = kDefaultInitialWindowSize;
    int64_t initial_stream_window = kDefaultInitialWindowSize;
    std::unordered_map<uint32_t, int64_t> stream_windows;
};

class H2FlowController {
public:
    std::expected<void, H2FlowControlError> ensureStream(uint32_t stream_id);
    std::expected<void, H2FlowControlError> applyConnectionWindowUpdate(uint32_t increment);
    std::expected<void, H2FlowControlError> applyStreamWindowUpdate(uint32_t stream_id, uint32_t increment);
    std::expected<void, H2FlowControlError> applyInitialStreamWindowSize(uint32_t new_size);
    size_t availableToSend(uint32_t stream_id, size_t requested, uint32_t max_frame_size) const;
    std::expected<void, H2FlowControlError> consumeSendWindow(uint32_t stream_id, size_t bytes);
};
```

**Step 3: 实现**

内部用 `int64_t` 算窗口，按 HTTP/2 最大窗口边界检查后再更新。

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t35_h2flow
rtk ctest --test-dir build -R '^http2\\.h2flow$' --output-on-failure
```

Expected: PASS。

## [ ] Task 7: out_sched 数据结构去搬移

**Files:**
- Modify: `src/galay-http2/kernel/out_sched.h`
- Modify: `src/galay-http2/kernel/out_sched.cc`
- Modify: `test/http2/t35_h2flow.cc`

**Step 1: 写 RED 测试**

覆盖：
- 大 body 多次调度后，剩余数据顺序正确。
- `pending_data` 不再通过 erase 前移。
- 空 body + `end_stream=true` 能产出零长度 END_STREAM frame。

**Step 2: 引入 pending buffer**

简化版结构：
```cpp
struct H2PendingData {
    std::deque<std::string> chunks;
    size_t front_offset = 0;
    bool end_stream = false;
};
```

`H2StreamSendState` 改为持有 `H2PendingData pending`。如需兼容旧测试，可短期保留 `pending_data` 并在构造测试中迁移，但最终应统一到 queue。

**Step 3: 实现切片**

从 `chunks.front()` + `front_offset` 生成 DATA frame payload，发送后推进 offset。只在当前 chunk 完全发送后 pop front。

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t35_h2flow
rtk ctest --test-dir build -R '^http2\\.h2flow$' --output-on-failure
```

Expected: PASS。

## [ ] Task 8: Deficit Round Robin 调度

**Files:**
- Modify: `src/galay-http2/kernel/out_sched.h`
- Modify: `src/galay-http2/kernel/out_sched.cc`
- Modify: `test/http2/t35_h2flow.cc`
- Optional Create: `test/http2/t86_h2out_scheduler.cc`

**Step 1: 写 RED 测试**

覆盖：
- 高权重流获得更多发送机会。
- 低权重流在多轮调度内仍能发送。
- 输入 `streams` 的物理顺序不被 `pickSendableFrames()` 排序破坏。

**Step 2: 引入调度状态**

```cpp
struct H2StreamSendState {
    uint32_t stream_id = 0;
    int32_t stream_window = 0;
    H2PendingData pending;
    uint8_t weight = 16;
    size_t deficit = 0;
    bool queued = false;
};

struct H2SchedulerConfig {
    size_t base_quantum = 16 * 1024;
};
```

**Step 3: 实现 DRR**

每轮：
- ready stream 的 `deficit += base_quantum * weight`
- 可发送大小取 `min(deficit, conn_window, stream_window, max_frame_size, pending_size)`
- 发送后扣 deficit/window
- 未完成则下轮继续

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t35_h2flow
rtk ctest --test-dir build -R '^http2\\.h2flow$' --output-on-failure
```

Expected: PASS。

## [ ] Task 9: 区分控制帧、HEADERS 与 DATA 队列

**Files:**
- Modify: `src/galay-http2/kernel/out_sched.h`
- Modify: `src/galay-http2/kernel/out_sched.cc`
- Modify: `test/http2/t35_h2flow.cc`

**Step 1: 写 RED 测试**

覆盖：
- SETTINGS ACK/PING ACK/RST_STREAM/GOAWAY 优先于 DATA。
- HEADERS 不受 DATA flow control 阻塞。
- DATA 在窗口为 0 时不发送。

**Step 2: 增加队列模型**

```cpp
struct H2OutboundQueues {
    std::deque<Http2Frame::uptr> control_frames;
    std::deque<Http2Frame::uptr> header_frames;
    std::vector<H2StreamSendState> data_streams;
};
```

**Step 3: 调整 selection**

`H2OutboundSelection` 先取 control，再取 headers，最后按 DRR 取 DATA。DATA 仍受 flow control 限制。

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t35_h2flow
rtk ctest --test-dir build -R '^http2\\.h2flow$' --output-on-failure
```

Expected: PASS。

## [ ] Task 10: h2_core 事件驱动接入

**Files:**
- Modify: `src/galay-http2/kernel/h2_core.h`
- Modify: `src/galay-http2/kernel/h2_core.cc`
- Modify: `test/http2/t33_h2core.cc`
- Modify: `test/http2/t36_h2timer.cc`

**Step 1: 写 RED 测试**

覆盖：
- 收到帧后调用 dispatcher，并把 actions 入 outbound queue。
- WINDOW_UPDATE 触发 out scheduler。
- control frame 入队后无需等待固定 1ms tick。
- timer 只负责 ping/settings/goaway 超时。

**Step 2: 去掉常规 1ms 轮询依赖**

保留 timer 检查，但常规发送由事件触发：
- inbound frame decoded
- new outbound data queued
- WINDOW_UPDATE
- socket writable/write completed
- control action queued

**Step 3: 保持 Task 不阻塞**

任何 socket I/O 都通过现有 async socket awaitable，不在 `Task<void>` 内做 blocking wait。

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t33_h2core t36_h2timer
rtk ctest --test-dir build -R '^http2\\.(h2core|h2timer)$' --output-on-failure
```

Expected: PASS。

## [ ] Task 11: 错误传播与关闭语义

**Files:**
- Modify: `src/galay-http2/kernel/h2_core.h`
- Modify: `src/galay-http2/kernel/h2_core.cc`
- Modify: `src/galay-http2/protoc/http2_error.h` only if existing error type cannot express needed errors
- Modify: `test/http2/t30_h2err.cc`
- Modify: `test/http2/t41_h2close.cc`

**Step 1: 写 RED 测试**

覆盖：
- 协议错误生成 GOAWAY 或 RST_STREAM，不抛异常。
- graceful GOAWAY 后停止接收新流。
- graceful timeout 后 force close。
- I/O 错误仍通过 kernel `IOError` 或连接关闭路径向上报告。

**Step 2: 定义核心错误边界**

如现有类型不足，新增轻量类型：
```cpp
struct H2CoreError {
    enum class Kind {
        Protocol,
        FlowControl,
        Io,
        Timeout,
    } kind;
    Http2ErrorCode h2_code = Http2ErrorCode::NoError;
    std::optional<galay::kernel::IOError> io_error;
};
```

**Step 3: 实现关闭状态**

连接状态至少区分：
- Running
- Draining
- Closing
- Stopped

**Step 4: 验证**

Run:
```bash
rtk cmake --build build --target t30_h2err t41_h2close
rtk ctest --test-dir build -R '^http2\\.(h2err|h2close)$' --output-on-failure
```

Expected: PASS。

## [ ] Task 12: 压力与回归验证

**Files:**
- Modify or Create: `benchmark/http2/*` only if the repository already has HTTP/2 benchmark registration enabled
- Modify: `docs/modules/http2/*` if public behavior changes

**Step 1: 增加压力覆盖**

建议覆盖：
- 1000 streams，每个 stream 小 DATA，验证公平性和无饥饿。
- 单 stream 大 body，验证无 `erase(0, chunk)` 级别退化。
- WINDOW_UPDATE 频繁到达，验证窗口不溢出。
- GOAWAY 期间已有流继续，新流拒绝。

**Step 2: 运行 HTTP/2 相关测试**

Run:
```bash
rtk cmake --build build --target t33_h2core t34_h2disp t35_h2flow t36_h2timer t30_h2err t41_h2close
rtk ctest --test-dir build -R '^http2\\.(h2core|h2disp|h2flow|h2timer|h2err|h2close)$' --output-on-failure
```

Expected: PASS。

**Step 3: 全量 HTTP/2 回归**

Run:
```bash
rtk ctest --test-dir build -R '^http2\\.' --output-on-failure
```

Expected: PASS。若已有无关失败，记录失败测试名、错误输出和是否与本次改动相关。

## 验收标准

- `frame_disp` 能明确区分 connection error、stream error、可继续分发动作。
- `frame_disp` 覆盖主要 frame stream id 规则、CONTINUATION 序列、GOAWAY 后新流拒绝、最小 stream lifecycle。
- `out_sched` 不再通过 `std::sort(streams)` 改变调用方顺序。
- `out_sched` 不再依赖 `std::string::erase(0, chunk)` 搬移大 body。
- DATA 受 connection/stream flow control 限制；控制帧和 HEADERS 不被 DATA 窗口阻塞。
- 多 stream 调度不会永久饿死低权重流。
- `h2_core` 不靠固定 1ms sleep 驱动常规发送。
- 所有新增错误通过 typed result/action 返回，不引入异常或进程退出路径。
- 相关 HTTP/2 CTest 通过；若全量 HTTP/2 测试存在无关失败，必须单独记录。

## 非目标

- 不实现完整 RFC 7540 priority dependency tree。
- 不引入 nghttp2 作为依赖。
- 不重写 HPACK。
- 不在本阶段重构 HTTP/2 client/server public API，除非测试证明现有 API 无法表达正确语义。
