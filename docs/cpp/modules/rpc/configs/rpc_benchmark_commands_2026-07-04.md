# RPC Benchmark 探测与补跑命令

本文件记录 2026-07-04 本轮 `galay-rpc` 压测归档使用的探测命令、结果摘要和后续补跑入口。本轮未执行长压测、未构建新二进制、未等待外部服务。

## 已执行的只读探测命令

`build/` 下：

```bash
rtk rg -n "^(CMAKE_BUILD_TYPE|GALAY_BUILD_BENCHMARKS|BUILD_BENCHMARKS|BUILD_TESTING|CMAKE_CXX_COMPILER|CMAKE_CXX_COMPILER_VERSION):" CMakeCache.txt
rtk rg -n "^(CMAKE_GENERATOR|CMAKE_MAKE_PROGRAM|CMAKE_BUILD_TYPE|GALAY_BUILD_BENCHMARKS|BUILD_TESTING|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS|CMAKE_EXE_LINKER_FLAGS):" CMakeCache.txt
rtk cmake --build . --target help
rtk proxy find /Users/gongzhijie/Desktop/projects/git/galay/build -type f -name 'benchmark_rpc*' -print
```

`build-release/` 下：

```bash
rtk rg -n "^(CMAKE_BUILD_TYPE|GALAY_BUILD_BENCHMARKS|BUILD_BENCHMARKS|BUILD_TESTING|CMAKE_CXX_COMPILER|CMAKE_CXX_COMPILER_VERSION):" CMakeCache.txt
rtk rg -n "^(CMAKE_GENERATOR|CMAKE_MAKE_PROGRAM|CMAKE_BUILD_TYPE|GALAY_BUILD_BENCHMARKS|BUILD_TESTING|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS|CMAKE_EXE_LINKER_FLAGS):" CMakeCache.txt
rtk cmake --build . --target help
rtk proxy find /Users/gongzhijie/Desktop/projects/git/galay/build-release -type f -name 'benchmark_rpc*' -print
```

工具与环境探测：

```bash
rtk which ghz
rtk ghz --version
rtk which grpc_cpp_plugin
rtk grpc_cpp_plugin --version
rtk pkg-config --modversion grpc++
rtk pkg-config --modversion grpc
rtk which protoc
rtk protoc --version
rtk which brpc
rtk pkg-config --modversion brpc
rtk which thrift
rtk thrift --version
rtk brew list --versions grpc protobuf brpc thrift ghz
rtk uname -a
rtk sysctl -n machdep.cpu.brand_string
rtk sysctl -n hw.ncpu
rtk sysctl -n hw.memsize
rtk /usr/bin/c++ --version
rtk pgrep -fl 'cmake|make|ninja|benchmark_rpc|ghz|grpc|brpc|thrift'
```

## 探测结果摘要

- `build/`：Debug，Ninja，`GALAY_BUILD_BENCHMARKS=OFF`，`BUILD_TESTING=OFF`，未发现 `benchmark_rpc*` 二进制。
- `build-release/`：Release，Unix Makefiles，`GALAY_BUILD_BENCHMARKS=ON`，`BUILD_TESTING=ON`，target help 中存在 `benchmark_rpc_*` targets，但未发现 `benchmark_rpc*` 二进制。
- `ghz`：不可用。
- `grpc_cpp_plugin` / `grpc++` / `grpc`：不可用。
- `brpc`：不可用。
- `thrift`：不可用。
- `protoc`：`libprotoc 29.3`。

## 后续补跑命令

所有命令执行时都需要保留 `rtk` 前缀。

```bash
rtk cmake --build build-release --target \
  benchmark_rpc_unary_loopback_latency \
  benchmark_rpc_stream_loopback_latency \
  benchmark_rpc_service_discovery_latency

rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_unary_loopback_latency 100
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_stream_loopback_latency -n 100 -s 128
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_service_discovery_latency -w 4 -d 1 -i 1
```

如需跑服务端/客户端 throughput 矩阵，先构建目标，再分别启动服务端和 1 秒短测客户端：

```bash
rtk cmake --build build-release --target \
  benchmark_rpc_unary_rpc_throughput \
  benchmark_rpc_streaming_rpc_throughput \
  benchmark_rpc_server_stream_throughput \
  benchmark_rpc_bidirectional_stream_throughput

rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_unary_rpc_throughput 9000 1 131072
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_streaming_rpc_throughput \
  -h 127.0.0.1 -p 9000 -c 4 -d 1 -s 128 -i 1 -l 1 -m unary
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_streaming_rpc_throughput \
  -h 127.0.0.1 -p 9000 -c 4 -d 1 -s 128 -i 1 -l 1 -m client_stream
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_streaming_rpc_throughput \
  -h 127.0.0.1 -p 9000 -c 4 -d 1 -s 128 -i 1 -l 1 -m server_stream
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_streaming_rpc_throughput \
  -h 127.0.0.1 -p 9000 -c 4 -d 1 -s 128 -i 1 -l 1 -m bidi

rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_server_stream_throughput 9100 1 131072
rtk ./build-release/benchmark/cpp/rpc/benchmark_rpc_bidirectional_stream_throughput \
  -h 127.0.0.1 -p 9100 -c 4 -d 1 -s 128 -f 4 -w 1 -i 1
```
