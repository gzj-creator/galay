# Example

目录结构（参考 galay-rpc）：

- `common/`：示例公共配置
- `include/`：direct-include 示例实现（E1~E14）
- `import/`：模块门禁满足时启用的 import 示例实现（TLS 场景还需要 `-DGALAY_BUILD_SSL=ON`）

保留示例类型：

- 各协议 Echo：HTTP / HTTPS / WS / WSS / H2c
- 各协议 Echo：HTTP / HTTPS / WS / WSS / H2c / H2(TLS)
- 静态服务器
- Proxy

`E12-HttpProxy` 支持 mount 集成：可以将某个前缀（默认 `/static`）作为本地静态目录，其余路径转发到 upstream。

构建：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --parallel
```

SSL 示例需启用：

```bash
cmake -S . -B build_ssl -DBUILD_EXAMPLES=ON -DGALAY_BUILD_SSL=ON
cmake --build build_ssl --parallel
```

模块 import 示例需满足真实模块门禁：

```bash
cmake -S . -B build-mod -G Ninja \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=OFF \
  -DBUILD_MODULE_EXAMPLES=ON \
  -DGALAY_BUILD_SSL=OFF
cmake --build build-mod --target galay-http T59-module_smoke --parallel
```

TLS import 示例额外需要：

```bash
cmake -S . -B build-mod-ssl -G Ninja \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARKS=OFF \
  -DBUILD_MODULE_EXAMPLES=ON \
  -DGALAY_BUILD_SSL=ON
cmake --build build-mod-ssl --target galay-http T59-module_smoke --parallel
```

说明：

- `import/` 在模块门禁满足时覆盖与 `include/` 树对齐的 E1~E14 场景
- 其中 `E5`~`E8`、`E13`、`E14` 还需要 `-DGALAY_BUILD_SSL=ON`
- `T59-module_smoke` 是模块 consumer 的最小验证入口，并在 TLS 构建中额外触达 `Https*` / `Wss*` / `H2*` builder

运行（Proxy + Mount）：

```bash
# 终端 1：upstream
./build/examples/E1-EchoServer 8080

# 终端 2：proxy，挂载 /static -> ./html（Nginx try_files 风格）
./build/examples/E12-HttpProxy 8081 127.0.0.1 8080 /static ./html nginx

# 走代理转发到 upstream
curl -X POST http://127.0.0.1:8081/echo -d "via proxy"

# 走本地 mount
curl http://127.0.0.1:8081/static/ResumeDownload.html
```
