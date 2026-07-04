# 2026-07-03 Redis benchmark rerun commands

All commands should be run from the repository root.

```bash
rtk cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGALAY_BUILD_BENCHMARKS=ON \
  -DGALAY_BUILD_REDIS=ON \
  -DGALAY_BUILD_SSL=ON \
  -DBUILD_TESTING=OFF

rtk cmake --build build-release --parallel \
  --target benchmark_redis_async_client_throughput \
           benchmark_redis_connection_pool_throughput

rtk redis-server --port 6379 --save "" --appendonly no

rtk redis-benchmark -h 127.0.0.1 -p 6379 \
  -c 50 -n 100000 -d 64 -P 1 -t get,set

rtk redis-benchmark -h 127.0.0.1 -p 6379 \
  -c 50 -n 100000 -d 64 -P 100 -t get,set

rtk ./build-release/benchmark/cpp/redis/benchmark_redis_async_client_throughput \
  -h 127.0.0.1 -p 6379 \
  -c 10 -n 10000 -m normal \
  --timeout-ms -1 --buffer-size 32768 -q

rtk ./build-release/benchmark/cpp/redis/benchmark_redis_async_client_throughput \
  -h 127.0.0.1 -p 6379 \
  -c 10 -n 50000 -m pipeline -b 100 \
  --timeout-ms -1 --buffer-size 32768 -q

rtk ./build-release/benchmark/cpp/redis/benchmark_redis_connection_pool_throughput \
  -h 127.0.0.1 -p 6379 \
  -c 20 -n 300 -m 4 -x 20 -q
```
