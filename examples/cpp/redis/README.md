# Example

目录结构（参考 galay-rpc / galay-http）：

- `common/`：示例公共配置
- `include/`：头文件消费示例（e1~e3）
- `import/`：模块消费示例（e1~e3，只有模块工具链可用时才生成 target）

构建：

```bash
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --parallel
```

运行：

```bash
./build/examples/e1_basic 127.0.0.1 6379
./build/examples/e2_pipeline 127.0.0.1 6379 demo:pipeline: 20
./build/examples/e3_pubsub 127.0.0.1 6379
./build/examples/e1_basic-import 127.0.0.1 6379
./build/examples/e2_pipeline-import 127.0.0.1 6379 demo:pipeline: 20
./build/examples/e3_pubsub-import 127.0.0.1 6379
```

如果当前工具链不支持仓库的 C++23 模块路径，`*-import` 目标会在配置阶段被自动跳过。

## 示例与模块对齐

- `e1_basic` ↔ `e1_basic-import`
- `e2_pipeline` ↔ `e2_pipeline-import`
- `e3_pubsub` ↔ `e3_pubsub-import`
