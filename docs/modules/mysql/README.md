# 文档索引

本页给出 `galay-mysql` 的规范阅读顺序。文件名、链接标签与页内一级标题保持一致，供人工阅读与 RAG 检索共同使用。

对外文档面收敛到 `README.md` + `docs/README.md` + `docs/00`~`docs/07`；历史报告与目录级说明不再作为独立检索入口保留。

## 推荐阅读顺序

1. [00-快速开始](00-快速开始.md) — 依赖、构建、安装、运行 tests/examples/benchmarks
2. [01-架构设计](01-架构设计.md) — 模块划分、异步 awaitable 模型、连接池与限制
3. [02-API参考](02-API参考.md) — 公开头文件、返回类型、配置结构、导出 target
4. [03-使用指南](03-使用指南.md) — 同步 / 异步 / 连接池 / 预处理语句的接入方式
5. [04-示例代码](04-示例代码.md) — `examples/` 真实文件、target 与运行命令
6. [05-性能测试](05-性能测试.md) — `benchmark/` 真实 target、参数与证据记录方式
7. [06-高级主题](06-高级主题.md) — 超时、buffer、零拷贝参数层、模块限制与生产注意事项
8. [07-常见问题](07-常见问题.md) — 常见错误、awaitable 语义、连接池与 benchmark FAQ

## 文档真相来源

文档内容与源码冲突时，按下列顺序取真：

1. 公开头文件与导出 target
2. 实现文件行为
3. `examples/`
4. `test/`
5. `benchmark/`
6. Markdown 文档

## 关键目录

- `galay-mysql/`：库源码与公开头
- `examples/`：真实示例源码与 target 定义
- `test/`：真实测试入口
- `benchmark/`：真实 benchmark 入口
- `cmake/`：构建开关与安装导出配置

## 文档自检命令

```bash
python3 scripts/verify_docs.py
```
