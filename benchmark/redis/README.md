# Benchmark

目录结构：

- `b1_client.cc`：RedisClient `normal` / `normal-batch` / `pipeline` 压测
- `b2_pool.cc`：连接池并发压测
- `b3_rediss.cc`：RedissClient `normal` / `pipeline` 压测

构建：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-release --parallel
```

## b1 参数

```bash
./build-release/benchmark/b1_client \
  [-h host] [-p port] [-c clients] [-n operations] \
  [-m normal|normal-batch|pipeline] [-b batch_size] \
  [--timeout-ms -1|N] [--buffer-size bytes] [--track-alloc] [-q]
```

- `--timeout-ms`: `-1` 表示不启用请求超时；`N>=0` 表示所有请求统一使用该超时。
- `--buffer-size`: 客户端 ring-buffer 大小，跨客户端对比时必须保持一致。
- `--track-alloc`: 启用分配统计（会引入额外开销，建议只在 allocation 分析时开启）。

当前 `b1` mode 对应的实现路径：

- `normal`：复用预编码 RESP 字节，通过 `RedisClient::commandBorrowed(...)` 发送。
- `pipeline`：复用 `RedisCommandBuilder::encoded()`，通过 `RedisClient::batchBorrowed(...)` 发送。
- `normal-batch`：继续走常规 `RedisClient::batch(...)`。

所以 plain `normal` / `pipeline` 的数字，表示当前 borrowed fast path 的吞吐，不等于所有 owning API 的泛化 baseline。

b1 输出包含：
- `Ops/sec`（吞吐）
- `Request latency p50/p99`（单次请求/批次调用延迟）
- `Alloc calls/op`、`Alloc bytes/op`（仅在 `--track-alloc` 时输出）

## 控制变量建议（跨语言公平对比）

1. timeout 两边要么都开、要么都关（推荐统一关闭：`--timeout-ms -1`）。  
2. buffer size 一致（例如统一 `32768` 或统一 `65536`）。  
3. batch size、clients、operations 一致。  
4. 每组至少跑 3 次，取中位数。  

示例（pipeline，无 timeout）：

```bash
./build-release/benchmark/b1_client \
  -h 127.0.0.1 -p 6379 -c 10 -n 50000 -m pipeline -b 100 \
  --timeout-ms -1 --buffer-size 32768 -q
```

示例（normal-batch，开启分配统计）：

```bash
./build-release/benchmark/b1_client \
  -h 127.0.0.1 -p 6379 -c 10 -n 50000 -m normal-batch -b 100 \
  --timeout-ms -1 --buffer-size 32768 --track-alloc -q
```

## Rust 对齐基准

Rust 同机同参对齐工具在 `benchmark/compare/rust/`：

```bash
bash benchmark/compare/rust/run_rust_alignment.sh --help
```

默认对齐路径是 `b1_client`（normal/pipeline）与 Rust 同参数客户端，便于快速判断 C++ 路径是否退化。

b2 示例：

```bash
./build-release/benchmark/b2_pool -h 127.0.0.1 -p 6379 -c 20 -n 300 -m 4 -x 20 -q
```
