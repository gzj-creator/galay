# SPSC 配对压测

该压测回答一个窄问题：在严格 `1P1C`、轮询、8 字节单调序列消息下，Galay 的
SPSC 专用数据面相对 Crossbeam 通用实现能获得多少吞吐收益。

## 对照边界

- `raw_bounded`：`galay::spsc::Ring<uint64_t>` 对
  `crossbeam_queue::ArrayQueue<u64>`。
- `unbounded`：`galay::spsc::UnboundedQueue<uint64_t>` 对
  `crossbeam_queue::SegQueue<u64>`。

两侧拥有相同的 1P1C 拓扑、消息生成器、容量、退避策略、计时边界和正确性门禁。
能力并不等价：Galay 类型明确放弃多生产者、多消费者和 waiter，Crossbeam 对照保留
通用 MPMC 语义。因此结果用于量化 SPSC 特化收益，不能解释为
“C++ 普遍快于 Rust”或同能力 API 的语言对比。

## 公平性门禁

- 每条消息必须保持 FIFO，接收数量和 64 位回绕 checksum 必须完全匹配。
- 两侧都在 worker ready 后开始计时，在 producer 和 consumer 都退出后停止计时。
- Python runner 先校准消息数，正式样本不得短于 1 秒，默认执行 15 个 ABBA 配对样本。
- C++ 与 Rust 必须报告一致的线程放置结果；macOS 只报告 `perf-class-only` 或
  `affinity-hint-only`，不会把 Darwin affinity tag 误报为绑核。
- 同一机器同一时刻只允许一个 SPSC paired runner，子进程有超时保护。
- 原始 JSON/CSV 保存每个校准、预热和正式样本、重试次数、命令及二进制 SHA-256。
- 默认通过条件为配对吞吐比 95% bootstrap CI 下界大于 `1.05`，且两侧样本 CV
  都不超过 `3%`；未满足时不得宣称稳定胜出。

## 运行

```bash
rtk cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release -DGALAY_BUILD_BENCHMARKS=ON
rtk cmake --build build-release --target benchmark_kernel_compare_spsc_paired -j
rtk cargo build --release --locked --manifest-path \
  benchmark/cpp/kernel/compare/rust-channel/Cargo.toml --bin spsc_paired
rtk python3 benchmark/cpp/kernel/compare/run_spsc_paired.py \
  --cpp-binary build-release/benchmark/cpp/kernel/benchmark_kernel_compare_spsc_paired \
  --rust-binary benchmark/cpp/kernel/compare/rust-channel/target/release/spsc_paired \
  --output-dir benchmark-results/spsc-cap4096 --capacity 4096
```

容量结论应分别运行 `64`、`256`、`4096` 和 `65536`，不能只挑选单个容量作为总括结论。
