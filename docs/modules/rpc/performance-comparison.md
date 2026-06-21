# RPC Performance Comparison

This document records the repeatable benchmark commands for `galay-rpc`.

```bash
rtk ./scripts/rpc_release_benchmark.sh
rtk ./scripts/rpc_compare_open_source.sh
```

The open-source comparison script uses a local gRPC C++ baseline when available.
If no baseline toolchain is installed, it records the exact blocker and still
emits the `galay-rpc` reference numbers.

## 2026-06-21 Release Run

Environment:

- Darwin gongzhijiedeMBP 25.3.0 arm64
- `requests=10000`
- `payload=128`

`rtk ./scripts/rpc_release_benchmark.sh` at 2026-06-21 22:43:30 CST:

| Workload | Result | Errors |
|---|---:|---:|
| Unary loopback | 23,285.8 ops/sec, p50/p90/p99 27/70/106 us | 0 |
| Stream pressure | 15,479,900 frames/sec | 0 |
| Concurrent unary pressure | 12,493,500 qps | 0 |
| Pool pressure | 5,136,220 qps | 0 |
| Managed client pressure | 18,131,000 qps | 0 |
| Payload scaling | 5,179,450 qps, 0.85 GB/s | 0 |

`rtk ./scripts/rpc_compare_open_source.sh`:

- `baseline=none`
- `status=blocked`
- `blocker=no local open-source C++ RPC baseline toolchain detected`

Because no local open-source C++ RPC baseline toolchain is installed, this run
does not claim a real framework-to-framework QPS comparison. It records the
blocker and the local `galay-rpc` reference workload.
