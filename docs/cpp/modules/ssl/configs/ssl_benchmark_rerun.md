# galay-ssl benchmark rerun config

本配置用于在当前 Debug build 之外补跑正式 Release 数据。所有命令都应从 `/Users/gongzhijie/Desktop/projects/git/galay/build` 执行，并带 `rtk` 前缀。

## Build

```bash
rtk cmake -S .. -B ../build-release -DCMAKE_BUILD_TYPE=Release -DGALAY_BUILD_BENCHMARKS=ON -DGALAY_BUILD_SSL=ON
rtk cmake --build ../build-release --parallel --target benchmark_ssl_tls_server_throughput benchmark_ssl_tls_client_throughput benchmark_ssl_tls_steady_state
```

## Galay TLS Echo

```bash
rtk ../build-release/benchmark/cpp/ssl/benchmark_ssl_tls_server_throughput 8443 ../test/cpp/ssl/certs/server.crt ../test/cpp/ssl/certs/server.key 4096 4
rtk ../build-release/benchmark/cpp/ssl/benchmark_ssl_tls_client_throughput 127.0.0.1 8443 50 200 47 4
rtk env GALAY_SSL_STATS=1 ../build-release/benchmark/cpp/ssl/benchmark_ssl_tls_client_throughput 127.0.0.1 8443 50 200 47 4
```

## OpenSSL / wrk

```bash
rtk openssl s_time -connect 127.0.0.1:8443 -time 30 -www /
rtk wrk -t4 -c50 -d30s --latency https://127.0.0.1:8443/files/1kb.bin
```

## Cipher Probe

```bash
rtk openssl ciphers -s -v -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256
rtk openssl ciphers -s -v -tls1_2 'ECDHE-RSA-AES128-GCM-SHA256'
```
