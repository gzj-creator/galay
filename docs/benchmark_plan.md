# Galay 框架性能压测计划

**版本**: v1.0  
**日期**: 2026-07-03  
**目标**: 全模块性能对比测试，验证 galay 在各协议/组件上的性能表现

---

## 1. 测试原则

### 1.1 控制变量
- **构建版本**: 所有测试均使用 Release 构建（`-DCMAKE_BUILD_TYPE=Release`）
- **线程数一致**: 服务端/客户端线程数保持相同
- **测试参数一致**: 连接数、请求数、并发数、预热时间等参数完全对齐
- **硬件环境一致**: 同一台机器、同一时间段测试
- **系统配置一致**: ulimit、TCP 参数、CPU affinity 等系统配置保持一致

### 1.2 测试指标
- **吞吐量**: req/s（每秒请求数）
- **延迟**: p50、p95、p99、p999（百分位延迟）
- **资源占用**: CPU 平均/峰值、内存峰值（RSS）
- **稳定性**: failed、errored、timeout 请求数
- **并发能力**: 最大并发连接数、最大并发流数

### 1.3 测试环境
```bash
# 系统配置检查清单
- OS: Linux (kernel 5.10+) 或 macOS
- CPU: 记录型号、核心数、频率
- Memory: 记录总内存
- ulimit -n: >= 65535
- net.core.somaxconn: >= 4096
- net.ipv4.tcp_max_syn_backlog: >= 4096
```

---

## 2. 模块压测计划

### 2.1 galay-kernel (协程运行时)

#### 2.1.1 竞品选择
- **tokio** (Rust)
- **libuv** (C)
- **Boost.Asio** (C++)

#### 2.1.2 测试场景
1. **任务调度延迟**
   - 测试: 10K/100K/1M 空任务调度时延
   - 指标: p50/p95/p99 调度延迟

2. **Channel 吞吐**
   - 测试: MPSC/MPMC channel 每秒消息数
   - 指标: msg/s、内存占用

3. **定时器精度**
   - 测试: 1K/10K/100K 定时器触发精度
   - 指标: 触发延迟分布

#### 2.1.3 压测参数
```bash
# 控制变量
RUNTIME_THREADS=4      # 运行时线程数
TASKS_COUNT=100000     # 任务数量
DURATION=30            # 持续时间（秒）
WARM_UP=5              # 预热时间（秒）
```

---

### 2.2 galay-http (HTTP/1.1)

#### 2.2.1 竞品选择
- **nginx** (1.24+)
- **httpd (Apache)** (2.4+)
- **caddy** (2.7+)

#### 2.2.2 测试场景
1. **静态文件服务**
   - 文件大小: 0B, 1KB, 16KB, 128KB, 1MB, 10MB
   - 工具: wrk / ab

2. **动态 API 响应**
   - 场景: JSON echo、简单计算、数据库查询
   - 工具: wrk

3. **并发连接**
   - Keep-alive vs 短连接
   - 最大并发连接数测试

#### 2.2.3 压测参数
```bash
# wrk 参数
WRK_THREADS=4          # wrk 线程数
WRK_CONNECTIONS=100    # 并发连接数
WRK_DURATION=30        # 持续时间
WRK_TIMEOUT=5          # 超时时间

# 服务端参数
SERVER_THREADS=4       # 服务端 IO 线程
SERVER_KEEP_ALIVE=60   # keep-alive 超时
```

#### 2.2.4 测试命令示例
```bash
# galay
./benchmark_http_server 8080 4 &
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8080/files/1kb.bin

# nginx
nginx -c nginx.conf &
wrk -t4 -c100 -d30s --latency http://127.0.0.1:8081/files/1kb.bin

# 确保 nginx.conf 配置：
# worker_processes 4;
# keepalive_timeout 60;
```

---

### 2.3 galay-http2 (HTTP/2)

#### 2.3.1 竞品选择
- **nghttpd** (nghttp2)
- **h2o**
- **nginx** (with HTTP/2)

#### 2.3.2 测试场景
1. **POST echo**（已有脚本）
   - 使用 h2load
   - 多路复用性能

2. **静态文件服务**
   - 0B, 1KB, 16KB, 128KB, 1MB
   - 测试多路复用优势

3. **并发流数**
   - 测试: max_streams=100/250/500/1000
   - 对比单连接多流性能

#### 2.3.3 压测参数（参考现有脚本）
```bash
# h2load 参数
H2LOAD_THREADS=4           # h2load 线程数
H2LOAD_CLIENTS=20          # 客户端连接数
H2LOAD_MAX_STREAMS=100     # 每连接最大流数
H2LOAD_DURATION=10         # 持续时间
H2LOAD_WARM_UP=2           # 预热时间

# 服务端参数
SERVER_IO_THREADS=4        # IO 线程数
SERVER_MAX_STREAMS=1000    # 最大流数

# Matrix 测试（寻找最佳配置）
SERVER_IO_THREADS_LIST="1 2 4 8"
H2LOAD_CLIENTS_LIST="20 40 80"
H2LOAD_MAX_STREAMS_LIST="100 250"
```

#### 2.3.4 测试命令（复用现有脚本）
```bash
# 单次对比
./scripts/http2/300_http2_h2load_compare.sh --post-echo

# Best-of matrix
BUILD_DIR=build-release \
./scripts/http2/300_http2_h2load_compare.sh --post-echo-best

# 静态文件
./scripts/http2/300_http2_h2load_compare.sh --static-files
```

---

### 2.4 galay-ws (WebSocket)

#### 2.4.1 竞品选择
- **websocketd**
- **gorilla/websocket** (Go)
- **ws** (Node.js)

#### 2.4.2 测试场景
1. **Echo 延迟**
   - 单连接 ping-pong 往返延迟
   - 工具: websocat

2. **广播吞吐**
   - 1 对 N 广播性能
   - N 个客户端同时接收

3. **并发连接**
   - 最大并发 WebSocket 连接数

#### 2.4.3 压测参数
```bash
# 测试参数
WS_CLIENTS=1000        # 并发客户端数
WS_MESSAGE_SIZE=1024   # 消息大小（字节）
WS_DURATION=30         # 持续时间
WS_SERVER_THREADS=4    # 服务端线程数
```

#### 2.4.4 测试工具
```bash
# websocat echo 延迟
for i in {1..10000}; do
  echo "ping" | websocat ws://127.0.0.1:8080/echo
done | ts '[%H:%M:%S.%.N]'

# 并发连接测试（需编写脚本）
python3 scripts/ws_bench.py \
  --url ws://127.0.0.1:8080 \
  --clients 1000 \
  --duration 30
```

---

### 2.5 galay-redis (Redis 客户端)

#### 2.5.1 竞品选择
- **redis-cli** (官方)
- **hiredis** (C)
- **redis-plus-plus** (C++)

#### 2.5.2 测试场景
1. **基础命令**
   - GET/SET/INCR/HGET/LPUSH
   - 工具: redis-benchmark

2. **Pipeline 吞吐**
   - Pipeline 深度: 10/50/100
   - 对比非 pipeline 性能

3. **连接池效率**
   - 测试连接池 vs 单连接

#### 2.5.3 压测参数
```bash
# redis-benchmark 参数
REDIS_CLIENTS=50       # 并发客户端数
REDIS_REQUESTS=100000  # 请求总数
REDIS_PIPELINE=1       # Pipeline 深度
REDIS_DATA_SIZE=64     # 数据大小（字节）

# galay 客户端参数
GALAY_POOL_SIZE=10     # 连接池大小
GALAY_THREADS=4        # 客户端线程数
```

#### 2.5.4 测试命令
```bash
# 启动 Redis 服务
redis-server --port 6379 --save "" --appendonly no

# redis-benchmark (官方基线)
redis-benchmark -h 127.0.0.1 -p 6379 \
  -c 50 -n 100000 -d 64 -P 1 -t get,set

# galay 客户端
./benchmark_redis_client \
  --host 127.0.0.1 --port 6379 \
  --clients 50 --requests 100000 \
  --data-size 64 --pipeline 1

# hiredis 对比
./benchmark_hiredis_client \
  --host 127.0.0.1 --port 6379 \
  --clients 50 --requests 100000 \
  --data-size 64 --pipeline 1
```

---

### 2.6 galay-mysql (MySQL 客户端)

#### 2.6.1 竞品选择
- **mysqlslap** (官方)
- **libmysqlclient** (C)
- **mysql-connector-cpp**

#### 2.6.2 测试场景
1. **简单查询**
   - SELECT 1
   - SELECT * FROM small_table (100 rows)

2. **Prepared Statement**
   - INSERT/UPDATE/DELETE 性能

3. **连接池效率**
   - 对比连接池 vs 短连接

#### 2.6.3 压测参数
```bash
# mysqlslap 参数
MYSQL_CONCURRENCY=50   # 并发连接数
MYSQL_ITERATIONS=1000  # 每连接迭代次数
MYSQL_ENGINE=InnoDB    # 存储引擎

# galay 客户端参数
GALAY_POOL_SIZE=10     # 连接池大小
GALAY_THREADS=4        # 客户端线程数
```

#### 2.6.4 测试命令
```bash
# mysqlslap (官方基线)
mysqlslap \
  --host=127.0.0.1 --port=3306 \
  --user=root --password=test \
  --concurrency=50 --iterations=1000 \
  --query="SELECT 1" \
  --create-schema=test

# galay 客户端
./benchmark_mysql_client \
  --host 127.0.0.1 --port 3306 \
  --user root --password test \
  --pool-size 10 --threads 4 \
  --query "SELECT 1" --iterations 50000
```

---

### 2.7 galay-mongo (MongoDB 客户端)

#### 2.7.1 竞品选择
- **mongoc** (C driver, 官方)
- **mongo-cxx-driver** (C++)

#### 2.7.2 测试场景
1. **文档插入**
   - 单文档、批量插入
   - 文档大小: 1KB/10KB/100KB

2. **查询性能**
   - find、findOne、aggregate

3. **连接池效率**

#### 2.7.3 压测参数
```bash
# 测试参数
MONGO_CLIENTS=50       # 并发客户端数
MONGO_DOCS=10000       # 文档数量
MONGO_DOC_SIZE=1024    # 文档大小（字节）
MONGO_POOL_SIZE=10     # 连接池大小
```

#### 2.7.4 测试命令
```bash
# mongoc 基线
./benchmark_mongoc_client \
  --uri mongodb://127.0.0.1:27017 \
  --clients 50 --docs 10000 \
  --doc-size 1024 --pool-size 10

# galay 客户端
./benchmark_mongo_client \
  --uri mongodb://127.0.0.1:27017 \
  --clients 50 --docs 10000 \
  --doc-size 1024 --pool-size 10
```

---

### 2.8 galay-etcd (etcd 客户端)

#### 2.8.1 竞品选择
- **etcdctl** (官方)
- **etcd-cpp-apiv3**

#### 2.8.2 测试场景
1. **KV 操作**
   - Put/Get/Delete 性能
   - 批量操作

2. **Watch 延迟**
   - Watch 通知延迟测试

3. **Lease 性能**

#### 2.8.3 压测参数
```bash
# 测试参数
ETCD_CLIENTS=10        # 并发客户端数
ETCD_REQUESTS=10000    # 请求数量
ETCD_KEY_SIZE=64       # key 大小
ETCD_VALUE_SIZE=256    # value 大小
```

#### 2.8.4 测试命令
```bash
# etcdctl benchmark
etcdctl \
  --endpoints=127.0.0.1:2379 \
  check perf \
  --load="put" \
  --clients=10 \
  --duration=30

# galay 客户端
./benchmark_etcd_client \
  --endpoints 127.0.0.1:2379 \
  --clients 10 --requests 10000 \
  --key-size 64 --value-size 256
```

---

### 2.9 galay-rpc (RPC 框架)

#### 2.9.1 竞品选择
- **gRPC** (C++)
- **brpc** (百度)
- **thrift** (Apache)

#### 2.9.2 测试场景
1. **一元调用**
   - 简单 echo RPC
   - 小/中/大消息

2. **流式调用**
   - Client streaming
   - Server streaming
   - Bidirectional streaming

3. **服务发现开销**

#### 2.9.3 压测参数
```bash
# 测试参数
RPC_CLIENTS=50         # 并发客户端数
RPC_REQUESTS=100000    # 请求总数
RPC_MESSAGE_SIZE=1024  # 消息大小（字节）
RPC_SERVER_THREADS=4   # 服务端线程数
```

#### 2.9.4 测试命令
```bash
# ghz (gRPC benchmark tool)
ghz --insecure \
  --proto ./proto/echo.proto \
  --call echo.Echo/SayHello \
  -d '{"message":"hello"}' \
  -c 50 -n 100000 \
  127.0.0.1:50051

# galay RPC
./benchmark_rpc_client \
  --server 127.0.0.1:50051 \
  --proto echo.proto \
  --method SayHello \
  --clients 50 --requests 100000 \
  --message-size 1024
```

---

### 2.10 galay-ssl (TLS)

#### 2.10.1 竞品选择
- **OpenSSL s_server/s_client** (基线)
- **Boost.Asio SSL**
- **nginx (HTTPS)**

#### 2.10.2 测试场景
1. **TLS 握手性能**
   - 握手次数/秒
   - 握手延迟

2. **TLS 吞吐**
   - HTTPS 静态文件
   - 对比非 TLS 开销

3. **加密套件对比**
   - TLS 1.2 vs 1.3
   - 不同 cipher suite

#### 2.10.3 压测参数
```bash
# 测试参数
SSL_CLIENTS=50         # 并发客户端数
SSL_CONNECTIONS=10000  # 握手次数
SSL_DURATION=30        # 持续时间
SSL_VERSION=TLSv1.3    # TLS 版本
SSL_CIPHER=TLS_AES_128_GCM_SHA256  # cipher suite
```

#### 2.10.4 测试命令
```bash
# OpenSSL s_time (握手性能)
openssl s_time \
  -connect 127.0.0.1:8443 \
  -time 30 \
  -www /index.html

# wrk (HTTPS 吞吐)
wrk -t4 -c50 -d30s --latency \
  https://127.0.0.1:8443/files/1kb.bin

# galay SSL
./benchmark_ssl_server 8443 4 &
wrk -t4 -c50 -d30s --latency \
  https://127.0.0.1:8443/files/1kb.bin
```

---

### 2.11 galay-tracing (链路追踪)

#### 2.11.1 竞品选择
- **Jaeger client** (C++)
- **OpenTelemetry C++**
- **Zipkin client**

#### 2.11.2 测试场景
1. **Span 创建开销**
   - 空 span 创建耗时
   - span 嵌套性能

2. **Exporter 吞吐**
   - OTLP HTTP/gRPC 导出性能
   - 批量导出效率

3. **Sampler 开销**
   - 不同采样策略的性能影响

#### 2.11.3 压测参数
```bash
# 测试参数
TRACE_SPANS=1000000    # span 数量
TRACE_SAMPLING=0.1     # 采样率
TRACE_BATCH_SIZE=100   # 批量导出大小
TRACE_THREADS=4        # 并发线程数
```

---

## 3. 测试执行计划

### 3.1 Phase 1: 环境准备（Day 1）
- [ ] 准备测试机器，记录硬件配置
- [ ] 安装所有竞品工具
- [ ] 配置系统参数（ulimit、TCP 参数）
- [ ] 构建 galay Release 版本
- [ ] 构建所有竞品 Release 版本
- [ ] 验证测试工具正常运行

### 3.2 Phase 2: HTTP/HTTP2 测试（Day 2-3）
- [ ] galay-http vs nginx/caddy/apache
- [ ] galay-http2 vs nghttpd/h2o/nginx
- [ ] galay-ws vs websocketd/gorilla/ws
- [ ] galay-ssl 握手/吞吐测试

### 3.3 Phase 3: 数据库客户端测试（Day 4-5）
- [ ] galay-redis vs hiredis/redis-plus-plus
- [ ] galay-mysql vs libmysqlclient/connector-cpp
- [ ] galay-mongo vs mongoc/mongo-cxx-driver
- [ ] galay-etcd vs etcd-cpp-apiv3

### 3.4 Phase 4: RPC 与运行时测试（Day 6-7）
- [ ] galay-rpc vs gRPC/brpc/thrift
- [ ] galay-kernel vs tokio/libuv/Boost.Asio
- [ ] galay-tracing vs Jaeger/OpenTelemetry

### 3.5 Phase 5: 数据整理与报告（Day 8）
- [ ] 汇总所有测试数据
- [ ] 生成性能对比图表
- [ ] 编写测试报告
- [ ] 识别性能瓶颈与优化点

---

## 4. 测试报告模板

### 4.1 单模块报告结构
```markdown
# {模块名称} 性能测试报告

## 测试环境
- CPU: 
- Memory: 
- OS: 
- galay 版本: 
- 竞品版本: 

## 测试场景 1: {场景名称}

### 测试参数
| 参数 | galay | 竞品A | 竞品B |
|------|-------|-------|-------|
| 线程数 | 4 | 4 | 4 |
| ... | ... | ... | ... |

### 测试结果
| 指标 | galay | 竞品A | 竞品B |
|------|-------|-------|-------|
| 吞吐量 (req/s) | | | |
| p50 延迟 (ms) | | | |
| p95 延迟 (ms) | | | |
| p99 延迟 (ms) | | | |
| CPU 平均 (%) | | | |
| CPU 峰值 (%) | | | |
| 内存峰值 (MB) | | | |

### 性能对比
- galay vs 竞品A: {X}% faster/slower
- galay vs 竞品B: {Y}% faster/slower

### 性能图表
[插入图表]

## 总结
- 优势: 
- 劣势: 
- 优化建议: 
```

---

## 5. 自动化脚本

### 5.1 测试框架结构
```
scripts/
├── benchmark/
│   ├── run_all.sh              # 运行所有测试
│   ├── http/
│   │   ├── bench_http.sh       # HTTP/1.1 对比测试
│   │   └── bench_http2.sh      # HTTP/2 对比测试（已有）
│   ├── ws/
│   │   └── bench_ws.sh         # WebSocket 对比测试
│   ├── database/
│   │   ├── bench_redis.sh      # Redis 对比测试
│   │   ├── bench_mysql.sh      # MySQL 对比测试
│   │   ├── bench_mongo.sh      # MongoDB 对比测试
│   │   └── bench_etcd.sh       # etcd 对比测试
│   ├── rpc/
│   │   └── bench_rpc.sh        # RPC 对比测试
│   ├── kernel/
│   │   └── bench_runtime.sh    # 运行时对比测试
│   └── utils/
│       ├── collect_metrics.sh  # 收集系统指标
│       └── generate_report.py  # 生成测试报告
```

### 5.2 测试脚本模板
```bash
#!/usr/bin/env bash
# scripts/benchmark/template.sh

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:---all}"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"

# 确保 Release 构建
BUILD_TYPE="$(cmake_build_type "$BUILD_DIR")"
if [[ "$BUILD_TYPE" != "Release" ]]; then
    echo "Error: Requires Release build" >&2
    exit 1
fi

# 控制变量参数
THREADS=4
CLIENTS=50
DURATION=30
WARM_UP=5

# 运行 galay 测试
run_galay_test() {
    local label="$1"
    # 实现测试逻辑
}

# 运行竞品测试
run_competitor_test() {
    local label="$1"
    local competitor="$2"
    # 实现测试逻辑
}

# 生成对比报告
generate_report() {
    # 实现报告生成
}

main() {
    run_galay_test "galay-${MODE}"
    run_competitor_test "competitor-a" "competitor-a"
    run_competitor_test "competitor-b" "competitor-b"
    generate_report
}

main "$@"
```

---

## 6. 注意事项

### 6.1 测试有效性检查
- [ ] 所有测试使用 Release 构建
- [ ] 控制变量参数已对齐
- [ ] 系统无其他高负载进程
- [ ] 测试结果可重复（运行 3 次取平均）
- [ ] 记录所有软件版本号

### 6.2 常见陷阱
1. **编译优化未开启**: 必须使用 `-O3` 或 `-O2`
2. **线程数不一致**: 服务端/客户端线程数必须相同
3. **系统限制**: ulimit、TCP 参数等系统限制
4. **Turbo Boost**: 确保 CPU 频率策略一致
5. **短连接 vs 长连接**: 必须明确测试场景
6. **预热不足**: 需要充分的预热时间

### 6.3 结果判定标准
- **显著优势**: > 20% 性能提升
- **基本持平**: ±10% 性能差异
- **明显劣势**: < -20% 性能下降

---

## 7. 交付物

### 7.1 测试报告
- [ ] 各模块性能测试报告（Markdown）
- [ ] 性能对比图表（PNG/SVG）
- [ ] 测试原始数据（CSV）
- [ ] 测试脚本与配置

### 7.2 性能看板
建议使用 Grafana + InfluxDB 实时展示性能指标：
- 吞吐量趋势
- 延迟分布
- 资源占用
- 竞品对比

---

## 8. 测试数据归档

### 8.1 数据存储结构
所有测试完成后，真实数据和机器配置必须归档到现有的模块文档中：

```
docs/
├── benchmark_plan.md                      # 本计划文档（统一入口）
├── machine_config.md                      # 测试机器配置（统一，新建）
├── cpp/modules/                           # C++ API 模块文档
│   ├── http/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   ├── benchmark_data/                # 新建：原始数据目录
│   │   │   ├── vs_nginx.csv
│   │   │   ├── vs_caddy.csv
│   │   │   └── charts/                    # 性能对比图表
│   │   │       ├── throughput.png
│   │   │       └── latency.png
│   │   └── configs/                       # 新建：测试配置
│   │       ├── galay.conf
│   │       └── nginx.conf
│   ├── http2/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   │       ├── vs_nghttpd.csv
│   │       ├── vs_h2o.csv
│   │       └── charts/
│   ├── ws/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── redis/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── mysql/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── mongo/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── etcd/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── rpc/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── kernel/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── ssl/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   ├── tracing/
│   │   ├── 05-性能测试.md                 # 更新真实数据
│   │   └── benchmark_data/
│   └── utils/
│       ├── 05-性能测试.md                 # 更新真实数据
│       └── benchmark_data/
└── c/modules/                             # C API 模块文档（结构同 cpp）
    ├── http/
    │   ├── 05-性能测试.md                 # 更新真实数据
    │   └── benchmark_data/
    ├── http2/
    │   ├── 05-性能测试.md                 # 更新真实数据
    │   └── benchmark_data/
    └── ...
```

**归档原则**：
- **05-性能测试.md**: 更新竞品对比的真实测试数据、机器配置引用、结论
- **benchmark_data/**: 存放原始 CSV 数据、性能对比图表、测试配置文件
- **machine_config.md**: 统一的机器配置文档，放在 `docs/` 根目录，所有模块引用

### 8.2 机器配置模板
每次测试前必须填写 `docs/benchmark_results/machine_config.md`：

```markdown
# 测试机器配置

## 硬件配置

### CPU
- 型号: {例如: Intel Xeon E5-2680 v4}
- 核心数: {物理核心数}
- 线程数: {逻辑线程数}
- 基础频率: {例如: 2.4 GHz}
- 最大频率: {例如: 3.3 GHz}
- 缓存: {L1/L2/L3 缓存大小}
- 架构: {例如: x86_64}

### 内存
- 总容量: {例如: 64 GB}
- 类型: {例如: DDR4}
- 频率: {例如: 2400 MHz}
- 通道: {例如: 双通道}

### 存储
- 类型: {SSD / HDD / NVMe}
- 容量: {例如: 512 GB}
- 型号: {例如: Samsung 970 EVO Plus}
- 读速度: {例如: 3500 MB/s}
- 写速度: {例如: 3300 MB/s}

### 网络
- 网卡: {例如: Intel X710}
- 带宽: {例如: 10 Gbps}
- 延迟: {本地回环延迟}

## 软件配置

### 操作系统
- 发行版: {例如: Ubuntu 22.04 LTS}
- 内核版本: {例如: 5.15.0-76-generic}
- 架构: {例如: x86_64}

### 编译器
- GCC 版本: {例如: 14.1.0}
- Clang 版本: {例如: 18.1.0}
- CMake 版本: {例如: 3.28.0}

### 依赖库
- OpenSSL 版本: {例如: 3.0.9}
- Boost 版本: {例如: 1.82.0}
- 其他关键依赖: {列出版本}

## 系统配置

### 内核参数
```bash
# ulimit
ulimit -n                    # {例如: 65535}
ulimit -u                    # {例如: 65535}

# TCP 参数
net.core.somaxconn           # {例如: 4096}
net.ipv4.tcp_max_syn_backlog # {例如: 4096}
net.ipv4.tcp_tw_reuse        # {例如: 1}
net.ipv4.ip_local_port_range # {例如: 10000 65535}
```

### CPU 调度
- Governor: {例如: performance}
- Turbo Boost: {enabled / disabled}
- CPU Affinity: {是否绑核}

### 关闭的服务
- 防火墙: {是/否}
- SELinux: {是/否}
- 其他后台服务: {列出}

## 测试时间
- 测试日期: {例如: 2026-07-03}
- 测试时段: {例如: 02:00-06:00 (低负载时段)}
- 系统负载: {测试期间系统平均负载}

## 环境变量
```bash
# 关键环境变量
LD_LIBRARY_PATH={路径}
PATH={路径}
{其他影响性能的环境变量}
```

## 构建配置

### galay 构建参数
```bash
cmake -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++-14 \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DGALAY_BUILD_BENCHMARKS=ON \
  -DBUILD_TESTING=ON
```

### 竞品构建参数
记录所有竞品的构建参数，确保公平对比。

## 验证信息
- 测试人员: {姓名}
- 审核人员: {姓名}
- 测试轮次: {第几次测试}
- 数据可重现性: {是否验证过可重现}
```

### 8.3 05-性能测试.md 更新模板

每个模块的 `05-性能测试.md` 应该按照以下结构更新竞品对比数据：

```markdown
# {模块名称} 性能测试

## 1. 内部性能测试

{保留现有的内部 kernel 层、单元性能测试}

---

## 2. 竞品对比测试

**测试环境**: 参考 [测试机器配置](../../../machine_config.md)

**测试日期**: {YYYY-MM-DD}  
**galay 版本**: {例如: v4.0.1}  
**测试人员**: {姓名}

### 2.1 竞品版本

| 实现 | 版本 | 构建配置 | 备注 |
|------|------|----------|------|
| galay | v4.0.1 | Release, -O3 | |
| {竞品A} | {版本} | Release, -O3 | |
| {竞品B} | {版本} | Release, -O3 | |

### 2.2 测试场景

#### 2.2.1 场景 1: {场景名称}

**测试描述**: {详细描述测试场景}

**控制变量**:
| 参数 | 值 | 说明 |
|------|-----|------|
| 服务端线程数 | 4 | 所有实现相同 |
| 客户端线程数 | 4 | 所有实现相同 |
| 并发连接数 | 100 | 所有实现相同 |
| 测试时长 | 30s | 所有实现相同 |
| 预热时长 | 5s | 所有实现相同 |

**测试命令**:

galay:
```bash
./benchmark_xxx_server 8080 4 &
{完整的测试命令}
```

{竞品A}:
```bash
{完整的启动命令}
{完整的测试命令}
```

**测试结果**:

| 指标 | galay | 竞品A | 竞品B | galay vs A | galay vs B |
|------|-------|-------|-------|------------|------------|
| 吞吐量 (req/s) | 125,430 | 98,742 | 110,256 | +27.0% | +13.8% |
| p50 延迟 (ms) | 0.82 | 1.05 | 0.91 | -21.9% | -9.9% |
| p95 延迟 (ms) | 1.24 | 1.89 | 1.45 | -34.4% | -14.5% |
| p99 延迟 (ms) | 2.15 | 3.42 | 2.68 | -37.1% | -19.8% |
| CPU 平均 (%) | 285.6 | 312.4 | 298.7 | -8.6% | -4.4% |
| 内存峰值 (MB) | 42.3 | 58.7 | 51.2 | -27.9% | -17.4% |
| failed 请求数 | 0 | 0 | 0 | - | - |

**性能图表**:

![吞吐量对比](./benchmark_data/charts/scenario1_throughput.png)
![延迟分布](./benchmark_data/charts/scenario1_latency.png)

**可重现性验证**:
| 轮次 | galay (req/s) | 竞品A (req/s) | 竞品B (req/s) |
|------|---------------|---------------|---------------|
| 第1次 | 125,430 | 98,742 | 110,256 |
| 第2次 | 124,896 | 98,521 | 109,874 |
| 第3次 | 125,712 | 99,103 | 110,532 |
| 平均值 | 125,346 | 98,789 | 110,221 |
| 标准差 | 412.3 | 294.5 | 340.8 |
| CV (%) | 0.33% | 0.30% | 0.31% |

**原始数据**: [vs_竞品A.csv](./benchmark_data/vs_competitor_a.csv), [vs_竞品B.csv](./benchmark_data/vs_competitor_b.csv)

**结论**:
- 吞吐量: galay 比竞品A 快 27.0%，比竞品B 快 13.8%
- 延迟: galay 的 p99 延迟比竞品A 低 37.1%，比竞品B 低 19.8%
- 资源占用: galay 的内存占用比竞品A 低 27.9%
- 稳定性: 所有实现均无失败请求，可重现性良好（CV < 0.5%）

#### 2.2.2 场景 2: {场景名称}
{重复上述结构}

### 2.3 综合分析

**性能优势**:
- {列出 galay 的性能优势点}
- {量化数据支撑}

**性能劣势**:
- {列出 galay 的性能劣势点（如有）}
- {量化数据支撑}

**资源效率**:
- CPU 效率: {分析}
- 内存效率: {分析}

### 2.4 性能瓶颈分析

**CPU 瓶颈**:
- {使用 perf / Instruments 等工具的分析结果}
- {热点函数占比}

**内存瓶颈**:
- {内存分配热点}
- {缓存命中率}

**IO 瓶颈**:
- {系统调用开销}
- {网络延迟}

### 2.5 优化建议

**短期优化**:
1. {具体优化点}
   - 预期收益: {例如: +10% 吞吐量}
   - 实现难度: {低/中/高}

**长期优化**:
1. {具体优化点}
   - 预期收益: {例如: +20% 吞吐量}
   - 实现难度: {低/中/高}

---

## 附录

### 测试配置文件
- galay 配置: [configs/galay.conf](./configs/galay.conf)
- 竞品配置: [configs/competitor.conf](./configs/competitor.conf)

### 测试脚本
- 测试脚本: [../../scripts/benchmark/{模块}/bench_{模块}.sh](../../scripts/benchmark/{模块}/bench_{模块}.sh)
```

### 8.4 数据归档检查清单

每个模块测试完成后，必须确保以下文件已归档：

**C++ 模块**:
- [ ] `docs/machine_config.md` - 测试机器配置（所有模块共用，首次测试时创建）
- [ ] `docs/cpp/modules/{模块}/05-性能测试.md` - 更新竞品对比章节（第 2 节）
- [ ] `docs/cpp/modules/{模块}/benchmark_data/*.csv` - 原始数据文件
- [ ] `docs/cpp/modules/{模块}/benchmark_data/charts/*.png` - 性能对比图表
- [ ] `docs/cpp/modules/{模块}/configs/*` - 测试配置文件
- [ ] `scripts/benchmark/{模块}/bench_{模块}.sh` - 测试脚本
- [ ] 所有竞品版本号已记录
- [ ] 可重现性已验证（至少 3 次测试）
- [ ] 报告已审核

**C API 模块**（如有独立压测需求）:
- [ ] `docs/c/modules/{模块}/05-性能测试.md` - 更新竞品对比章节
- [ ] `docs/c/modules/{模块}/benchmark_data/*.csv` - 原始数据文件
- [ ] `docs/c/modules/{模块}/benchmark_data/charts/*.png` - 性能对比图表

### 8.5 自动化归档脚本

在 `scripts/benchmark/utils/` 中提供自动归档工具：

```bash
# 归档单个模块测试结果到对应的 05-性能测试.md
./scripts/benchmark/utils/archive_results.sh \
  --module http2 \
  --api cpp \
  --raw-data /tmp/http2_test_output.txt

# 生成性能对比图表
./scripts/benchmark/utils/generate_charts.py \
  --module http2 \
  --api cpp \
  --data docs/cpp/modules/http2/benchmark_data/vs_nghttpd.csv \
  --output docs/cpp/modules/http2/benchmark_data/charts/

# 验证数据完整性
./scripts/benchmark/utils/verify_results.sh \
  --module http2 \
  --api cpp

# 批量归档所有模块
./scripts/benchmark/utils/archive_all.sh --api cpp
```

**归档脚本功能**:
- 解析原始测试输出，提取关键指标
- 自动计算性能对比百分比
- 生成 Markdown 表格和图表
- 更新对应模块的 `05-性能测试.md` 文件
- 验证数据完整性和可重现性

---

## 9. 参考资料

### 8.1 竞品文档
- nginx: https://nginx.org/en/docs/
- nghttpd: https://nghttp2.org/documentation/
- gRPC: https://grpc.io/docs/
- Redis: https://redis.io/docs/
- MySQL: https://dev.mysql.com/doc/

### 8.2 压测工具
- wrk: https://github.com/wg/wrk
- h2load: https://nghttp2.org/documentation/h2load.1.html
- redis-benchmark: https://redis.io/docs/management/optimization/benchmarks/
- mysqlslap: https://dev.mysql.com/doc/refman/8.0/en/mysqlslap.html
- ghz (gRPC): https://ghz.sh/

---

## 9. 版本历史

| 版本 | 日期 | 变更说明 |
|------|------|----------|
| v1.0 | 2026-07-03 | 初始版本，覆盖全部 13 个模块 |

---

**负责人**: {填写}  
**审核人**: {填写}  
**预计完成日期**: {填写}
