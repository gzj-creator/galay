# Galay kernel benchmark probe summary

- Date: 2026-07-03 23:49:26 CST
- Workspace: `/Users/gongzhijie/Desktop/projects/git/galay/build`

## Plan sections read

- `docs/benchmark_plan.md` 2.1: galay-kernel competitors are tokio, libuv, Boost.Asio.
- `docs/benchmark_plan.md` 2.1: required scenarios are task scheduler latency, channel throughput, and timer precision.
- `docs/benchmark_plan.md` 4/7/8: module report, raw CSV, configuration/log archive, and machine configuration reference are required.

## Current build probe

- `CMAKE_BUILD_TYPE=Debug`
- `BUILD_TESTING=OFF`
- `GALAY_BUILD_BENCHMARKS=OFF`
- `GALAY_BUILD_C_API=OFF`
- `GALAY_BUILD_KERNEL=ON`
- `GALAY_DISABLE_IOURING=ON`
- Generator: Ninja

Commands and results:

- `rtk rg -n "BENCH|BUILD_TEST|BUILD_TESTING|CMAKE_BUILD_TYPE|CMAKE_GENERATOR|CMAKE_MAKE_PROGRAM|CMAKE_CXX_COMPILER|CMAKE_C_COMPILER|ENABLE|GALAY" CMakeCache.txt`
  - Result: current build is Debug and benchmark targets are disabled.
- `rtk cmake --build . --target help | rtk rg 'benchmark_kernel|benchmark|kernel'`
  - Result: only `galay-kernel` library/install targets were listed; no kernel benchmark targets were present.
- `rtk rg -n "benchmark_kernel_" build.ninja`
  - Result: no `benchmark_kernel_*` target was present in the current Ninja graph.
- `rtk proxy find . -maxdepth 6 -type f -perm -111 -print | rtk rg '(^|/)B[0-9]|benchmark|kernel'`
  - Result: only `./build/src/cpp/galay-kernel/libgalay-kernel.dylib` was found; no benchmark binary was available.

## Benchmark source mapping

- `benchmark/cpp/kernel/CMakeLists.txt` maps `b1_compute_scheduler_throughput.cc` to `benchmark_kernel_compute_scheduler_throughput`.
- `benchmark/cpp/kernel/CMakeLists.txt` maps `b8_mpsc_channel_throughput.cc` to `benchmark_kernel_mpsc_channel_throughput`.
- `benchmark/cpp/kernel/CMakeLists.txt` maps `b14_scheduler_wakeup_latency.cc` to `benchmark_kernel_scheduler_wakeup_latency`.
- `benchmark/cpp/kernel/CMakeLists.txt` maps `b18_ready_entry_wakeup_latency.cc` to `benchmark_kernel_ready_entry_wakeup_latency`.
- `benchmark/cpp/kernel/CMakeLists.txt` maps `b20_thread_safe_timer_manager.cc` to `benchmark_kernel_thread_safe_timer_manager`.
- `benchmark/c/kernel/CMakeLists.txt` maps `b24_coro_sleep_latency.c` to `benchmark_c_kernel_coro_sleep_latency` when C API benchmarks are enabled.

## Competitor probe

- tokio: `rustc 1.91.1` and `cargo 1.91.1` are present, but no tokio competitor benchmark source or binary was found.
- libuv: `pkg-config --modversion libuv` and Homebrew both report `1.51.0`, but no libuv competitor benchmark source or binary was found.
- Boost.Asio: Homebrew/package-config/CMake did not find Boost, and `boost/asio.hpp` was not found under `/opt/homebrew/include`, `/usr/local/include`, or `/usr/include`.

## Environment snapshot

- OS: macOS 26.3.1 (a), Darwin 25.3.0, arm64
- CPU: Apple M4 Pro, 12 physical CPUs, 12 logical CPUs
- Memory: 51539607552 bytes
- Compiler: Apple clang 17.0.0
- Git version: `v4.0.1-2-gfd86efe-dirty`

## Blocked reason

Current build directory is Debug and has `GALAY_BUILD_BENCHMARKS=OFF`, so Release benchmark data was not collected. No current-build kernel benchmark target or binary was available to run. Competitor baselines were not run because no tokio/libuv/Boost.Asio harness or binary exists in this checkout, and Boost.Asio is not installed locally.

## Suggested re-run commands

```bash
rtk cmake -S .. -B ../build-kernel-bench-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGALAY_BUILD_BENCHMARKS=ON \
  -DGALAY_BUILD_KERNEL=ON \
  -DGALAY_BUILD_C_API=ON \
  -DBUILD_TESTING=OFF

rtk cmake --build ../build-kernel-bench-release \
  --target benchmark_kernel_compute_scheduler_throughput \
           benchmark_kernel_mpsc_channel_throughput \
           benchmark_kernel_scheduler_wakeup_latency \
           benchmark_kernel_ready_entry_wakeup_latency \
           benchmark_kernel_thread_safe_timer_manager \
           benchmark_c_kernel_coro_sleep_latency \
  --parallel

rtk ../build-kernel-bench-release/benchmark/cpp/kernel/benchmark_kernel_compute_scheduler_throughput 4
rtk ../build-kernel-bench-release/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput
rtk ../build-kernel-bench-release/benchmark/cpp/kernel/benchmark_kernel_scheduler_wakeup_latency
rtk ../build-kernel-bench-release/benchmark/cpp/kernel/benchmark_kernel_ready_entry_wakeup_latency
rtk ../build-kernel-bench-release/benchmark/cpp/kernel/benchmark_kernel_thread_safe_timer_manager
rtk ../build-kernel-bench-release/benchmark/c/kernel/benchmark_c_kernel_coro_sleep_latency
```
