# galay-ssl 性能测试

本目录包含 `galay-ssl` 的压测程序，用于评估 SSL/TLS Echo 场景的 QPS、吞吐量和错误率。

## 测试程序

### b1_server

SSL Echo 服务端。

```bash
./build/bin/b1_server <port> <cert_file> <key_file> [backlog] [worker_count]
```

参数：

- `port`：监听端口
- `cert_file`：证书路径
- `key_file`：私钥路径
- `backlog`：可选，默认 `4096`
- `worker_count`：可选，默认 `1`；大于 `1` 时启用 `SO_REUSEPORT + 多 scheduler + 多 listener`

示例：

```bash
./build/bin/b1_server 8443 certs/server.crt certs/server.key
./build/bin/b1_server 8443 certs/server.crt certs/server.key 4096 4
```

### b1_client

SSL 压测客户端。

```bash
./build/bin/b1_client <host> <port> <connections> <requests_per_conn> [payload_bytes] [threads] [connect_retries]
```

参数：

- `host`：服务端地址
- `port`：服务端端口
- `connections`：并发连接数
- `requests_per_conn`：每连接请求数
- `payload_bytes`：可选，单次 payload 大小，默认 `47`
- `threads`：可选，客户端线程数，默认 `1`
- `connect_retries`：可选，连接重试次数，默认 `3`

示例：

```bash
# 小包（默认 47B）
./build/bin/b1_client 127.0.0.1 8443 200 500 47 4

# 大包（64KiB）
./build/bin/b1_client 127.0.0.1 8443 10 200 65536 1
```

## 推荐流程

```bash
# 1) 生成证书（如需要）
bash test/certs/generate_certs.sh

# 2) 启动服务端
./build/bin/b1_server 8443 certs/server.crt certs/server.key 4096 4

# 3) 启动客户端（另一个终端）
./build/bin/b1_client 127.0.0.1 8443 200 500 47 4
```

## 输出指标

客户端输出：

- `Total requests`
- `Total errors`
- `Error breakdown`（仅当错误 > 0）
- `Total bytes sent/received`
- `Duration`
- `Requests/sec`
- `Throughput`

可选统计（仅 benchmark 侧）：

```bash
GALAY_SSL_STATS=1 ./build/bin/b1_client 127.0.0.1 8443 50 200 47 4
```

会额外输出：

- `Send ops / send plain bytes`
- `Recv ops / recv plain bytes / recv chunks`
- `Avg recv chunk bytes`

## 注意事项

- 测试证书为自签名，默认用于开发/测试
- 客户端默认 `SslVerifyMode::None`（压测口径），不是生产安全配置
- 建议统一使用 `Release + LTO`，避免构建口径造成数据偏差
- 默认远端 CPU 绑定建议是 `server=0-1`、`client=2-3`；如果远端规格不同，请按实际 CPU 拓扑调整

## Rust 最小对照

仓库提供 Rust TLS echo 对照实现（用于同场景 benchmark 对比）：

- `benchmark/compare/rust/src/bin/rust_ssl_bench_server.rs`
- `benchmark/compare/rust/src/bin/rust_ssl_bench_client.rs`

一键对比命令：

```bash
./scripts/run.sh bench-compare
```

该命令会先跑 C++ benchmark，再跑 Rust benchmark，并输出 `Requests/sec` 与 `Throughput` 对比摘要。
