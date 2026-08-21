# RPC Benchmarks

正式外部对标只采用 Boost.Asio C++ 协程。RPC 当前没有同 wire、同 workload 的
Asio 协程 harness，因此 `302_rpc_compare_open_source.sh` 只输出
`not_applicable`；gRPC 等资料属于历史/内部参考，不参与竞品排名。

Release-mode benchmark entrypoints:

- `scripts/rpc/301_rpc_release_benchmark.sh`
- `scripts/rpc/302_rpc_compare_open_source.sh`（政策状态检查）

Set `RPC_BENCH_REQUESTS` and `RPC_BENCH_PAYLOAD` to adjust the short matrix.
Raw outputs are written under `benchmark/results/rpc/`.
