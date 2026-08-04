# SPSC 配对压测

该压测只回答一个问题：在同一台机器上，以严格 `1P1C` 传递 8 字节单调序列时，
Galay 与 Rust 专用 SPSC 实现的所测数据路径吞吐分别是多少。它不是通用的 C++/Rust
语言基准，也不把 MPMC 实现伪装成 SPSC 对照。

正式胜负只使用接口等价的 `raw_bounded` 与 `batch_bounded`。`unbounded` 是最接近的
专用 SPSC 数据路径，`batch_unbounded` 则因 Rust 依赖没有原生 batch API 而仅作为参考；
两者都不进入“全面超过”的结论。

## 对照边界

| case | Galay | Rust | `comparison_scope` |
|---|---|---|---|
| `raw_bounded` | `galay::spsc::Ring<uint64_t>::split()` | `rtrb::RingBuffer<u64>` `0.3.4` | `equivalent_measured_api` |
| `batch_bounded` | `Ring::split()` endpoint `tryWriteBatch` / `tryReadBatch` | `push_partial_slice` / `pop_partial_slice` | `equivalent_measured_api` |
| `unbounded` | `galay::spsc::UnboundedChannel<uint64_t>` | `unbounded_spsc::channel<u64>()` `0.3.0` | `nearest_available_measured_path` |
| `batch_unbounded` | `UnboundedChannel::sendBatch` / `tryRecvBatch` | 预分配 batch buffer 后逐条 `send` / `try_recv` | `reference_only_no_equivalent_rust_batch_api` |

`raw_bounded` 两侧都是预分配、固定容量、拆分 producer/consumer 的 polling SPSC ring。
容量都表示可同时保存的元素数；满时发送失败且值未被消费，空时接收立即失败。两侧
被测 API 等价，但完整生命周期能力仍不同：Rust endpoint 通过 `Arc` 持有 ring 并能
查询对端是否已销毁，Galay endpoint 借用 ring，要求 ring 活得更久。因此这里的
`equivalent_measured_api` 不表示两个 crate 的全部公开 API 完全相同。

`batch_bounded` 使用同一 ring、容量和最大 batch 大小；C++ 通过 `Ring::split()` 让两个
worker 各自独占本地 cursor。最后不足一个 batch 的消息仍须全部发送和接收；当 batch
大于 capacity 时，两侧都允许一次只发布或接收可用前缀，并继续处理剩余部分。只有返回
`0`/无可用槽位的调用计为 retry，部分批次不计 retry，并保持与逐条 case 相同的 FIFO、
接收数和 64 位回绕 checksum 校验。

`unbounded` 选择 crates.io 上专用的 unbounded SPSC channel，而不是 Crossbeam MPMC。
两侧测量的都是非阻塞 `send`/`try-receive` 路径，也都提供等待能力，但完整语义不等价：
Galay 使用 coroutine waiter/timeout，Rust 使用阻塞 `recv`/timeout 并额外检查 disconnect；
两者的分块和扩容策略也是各自实现的一部分。因此该 case 只能作为最接近的专用 SPSC
数据路径对照，不能据此宣称完整 channel API 等价。

`batch_unbounded` 的 C++ 侧调用现有原生批处理 API，并在发送侧转移一个预分配、循环
复用的 vector；Rust `unbounded-spsc 0.3.0` 没有等价的 batch API，只能使用预分配
buffer 逐条模拟。runner 会保存该参考 case 的非零退出码和无效 JSON；任何丢失、重复、
乱序、checksum 不匹配或 final drain 不完整都会让正确性门禁失败，失败样本不会被静默
丢弃，也不能用于正式胜负。

Rust 依赖由 `Cargo.lock` 精确锁定为 `rtrb 0.3.4`、`unbounded-spsc 0.3.0` 和其传递依赖
`bounded-spsc-queue 0.4.0`。SPSC binary 不引用 `crossbeam_queue`；Cargo 中保留的
Crossbeam 依赖仅供同目录其他 benchmark binary 使用。

## 公平性门禁

- 两侧使用相同的 `1P1C`、`uint64_t/u64`、消息生成器、容量、batch 大小和退避策略。
  `--backoff` 可选 `yield`、`spin`、`hybrid`，runner 会把同一值传给两侧并逐条校验输出；
  在 Linux 正式对照完成前默认仍为 `yield`，不基于 macOS 噪声切换默认值。
- channel/ring 构造和首块分配在计时前完成；计时从两个 worker ready 后统一放行开始，
  到 producer 与 consumer 都完成并退出为止。
- C++ worker 与 Rust worker 都在线程本地累计校验和、FIFO 与重试计数，只在线程结束时
  发布结果，避免把 benchmark 统计写入误计为 channel 数据面成本。
- consumer 必须按预期消息数完成最终 drain；FIFO、接收数和 64 位回绕 checksum 必须
  完全匹配，不能用 producer 完成后的单次空读提前结束。Rust unbounded 参考路径在
  producer 完成后继续 drain；若 1 秒没有进展则输出无效结果并正常终止，避免无限等待。
- C++ 和 Rust 进程严格串行运行。runner 先校准同一消息数；接口等价 case 的校准时长
  下限为 `target_seconds * 3`，参考 case 为 `target_seconds`。随后预热并默认执行
  15 个 ABBA 配对样本，每个正式样本都不得短于 `target_seconds`。
- 两侧必须报告一致的线程放置结果。Linux 使用真实绑核；macOS 只报告
  `perf-class-only` 或 `affinity-hint-only`，不会把 Darwin affinity tag 误报为绑核。
- 只有 producer/consumer 在两侧所有正式样本中都报告 `pinned` 时，
  `placement_passed` 才成立。任何非 `pinned` 样本只能作为噪声观测，不允许用于任何方向
  的胜负结论。
- runner 通过进程锁禁止并发 SPSC paired run，并保存每个校准、预热、正式样本、
  重试次数、命令、二进制 SHA-256 和两侧编译器版本。
- 校准的最短时长安全底线为 `target_seconds * 3`，用于吸收短时频率波动；正式样本仍
  必须逐个不少于 `target_seconds`。
- 当前两边都使用各自标准 Release 优化：CMake Release 为 `-O3 -DNDEBUG`，Cargo
  release 为 `opt-level=3`；两边都不额外启用 LTO、PGO 或 native CPU 指令目标。
- schema `galay.spsc.paired.v4` 的 `implementation`、`api_profile`、
  `comparison_scope`、`batch_size` 和 `backoff` 会由 runner 按 case 精确校验；旧 v2
  或 v3 结果直接拒绝，防止混用协议或悄悄扩大结论。

默认稳定胜出门槛同时要求：配对吞吐比的 95% bootstrap CI 下界大于 `1.05`，且 C++、
Rust 两侧样本 CV 都不超过 `25%`、线程放置全部为 `pinned`、两侧 `empty_retries` 中位数
比值不超过 `10`。任一条件失败时，只能报告观测中位数和噪声，不能宣称稳定胜出。
runner 同时输出两侧 QPS p50/p99、CV、median bootstrap 95% CI，以及 full、empty、
total retry ratio 的 p50/p99 与 CI。跨参数结论必须完整覆盖 batch size
`8/16/32/64/128/256` 和 capacity `64/256/4096/65536`，不能挑选单个组合概括成
“全面超过”。

## 构建与运行

```bash
rtk cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release -DGALAY_BUILD_BENCHMARKS=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
rtk cmake --build build-release \
  --target benchmark_kernel_compare_spsc_paired -j
rtk cargo +nightly build --release --locked --offline \
  --manifest-path benchmark/cpp/kernel/compare/rust-channel/Cargo.toml \
  --bin spsc_paired
rtk python3 benchmark/cpp/kernel/compare/run_spsc_paired.py \
  --cpp-binary build-release/benchmark/cpp/kernel/benchmark_kernel_compare_spsc_paired \
  --rust-binary benchmark/cpp/kernel/compare/rust-channel/target/release/spsc_paired \
  --output-dir benchmark-results/spsc-cap4096-yield \
  --capacity 4096 --batch-size 64 --backoff yield
```

Rust 的 `unbounded-spsc 0.3.0` 使用 nightly feature，因此必须显式使用 `+nightly`。
runner 会自动记录 `c++ --version` 与 `rustc +nightly -vV`，并生成 raw JSON、CSV 和
summary JSON。正式 Linux 对照必须固定两个不同 CPU，使用相同 backoff，并为每组
capacity/batch 参数使用独立输出目录，不得让后一次结果覆盖前一次结果。

单线程数据面回归项独立构建和运行，不参与 C++/Rust 胜负判断：

```bash
rtk cmake --build build-release --target benchmark_kernel_spsc_ring_pingpong -j
rtk proxy ./build-release/benchmark/cpp/kernel/benchmark_kernel_spsc_ring_pingpong
```

该目标固定使用 capacity `4096`、5000 万次往返和 3 轮中位数，并输出每轮
`round_trip_ns`、`round_trips_per_second` 及相对 `0.71-0.89 ns` 基线的 30% 回归判断。
绝对阈值只用于同一硬件的纵向比较；跨机器变慢不会让 benchmark 进程失败，正确性失败才会
返回非零。
