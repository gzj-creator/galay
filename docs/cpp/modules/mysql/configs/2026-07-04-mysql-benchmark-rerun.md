# 2026-07-04 MySQL Benchmark 补跑命令

本文件只记录补跑入口。本次未启动构建、长压测或写入类 SQL。

## 前置检查

```bash
rtk rg -n "^(CMAKE_BUILD_TYPE|GALAY.*BENCHMARK|BUILD_TESTING|CMAKE_GENERATOR):" ../build-release/CMakeCache.txt
rtk cmake --build ../build-release --target help | rtk rg "benchmark_mysql_"
rtk proxy find ../build-release -maxdepth 5 -type f -perm -111 \( -name '*mysql*benchmark*' \) -print
rtk mysql --connect-timeout=2 --protocol=TCP -h127.0.0.1 -P3306 -NBe "SELECT 1"
```

## 构建 Release benchmark

```bash
rtk cmake --build ../build-release --target benchmark_mysql_sync_query_pressure benchmark_mysql_async_query_pressure benchmark_mysql_async_pool_pressure benchmark_mysql_async_pool_lease_pressure --parallel
```

## 环境变量

不要把真实密码写入仓库。补跑时在本地 shell 设置：

```bash
export GALAY_MYSQL_HOST=127.0.0.1
export GALAY_MYSQL_PORT=3306
export GALAY_MYSQL_USER=<set-locally>
export GALAY_MYSQL_PASSWORD=<set-locally>
export GALAY_MYSQL_DB=<dedicated-benchmark-db>
```

## 短查询 smoke

```bash
rtk ../build-release/benchmark/cpp/mysql/benchmark_mysql_sync_query_pressure --clients 1 --queries 1 --warmup 1 --timeout-sec 5 --sql "SELECT 1"
rtk ../build-release/benchmark/cpp/mysql/benchmark_mysql_async_query_pressure --clients 1 --queries 1 --warmup 1 --timeout-sec 5 --sql "SELECT 1"
```

## 正式补跑

正式压测前先确认专用测试库、隔离账号、机器配置和原始 stdout 归档策略。不要在业务库上运行写入类 Prepared Statement 压测。

```bash
rtk ../build-release/benchmark/cpp/mysql/benchmark_mysql_sync_query_pressure --clients 50 --queries 1000 --warmup 100 --timeout-sec 180 --sql "SELECT 1"
rtk ../build-release/benchmark/cpp/mysql/benchmark_mysql_async_query_pressure --clients 50 --queries 1000 --warmup 100 --timeout-sec 180 --sql "SELECT 1"
rtk ../build-release/benchmark/cpp/mysql/benchmark_mysql_async_pool_pressure --clients 50 --queries 1000 --warmup 100 --timeout-sec 180 --batch-size 10 --sql "SELECT 1"
rtk ../build-release/benchmark/cpp/mysql/benchmark_mysql_async_pool_lease_pressure --clients 50 --queries 1000 --warmup 100 --timeout-sec 180 --batch-size 10 --sql "SELECT 1"
```
