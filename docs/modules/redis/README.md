# 文档索引

本目录是 `galay-redis` 的规范化阅读入口。编号、文件名和主题顺序均与真实文件集保持一致。

## 推荐阅读顺序

| 文件 | 主题 | 主要真相来源 |
|---|---|---|
| [00-快速开始](00-快速开始.md) | 编译、第一条命令、最短验证路径 | `CMakeLists.txt`、`examples/include/e1_basic.cc`、`test/t1_async.cc` |
| [01-架构设计](01-架构设计.md) | 模块分层、执行链路、拓扑与连接池 | `galay-redis/async/*.h`、`galay-redis/protoc/*.h` |
| [02-API参考](02-API参考.md) | 当前安装头文件、模块导出面与目标名 | `galay-redis/**/*.h`、`galay-redis/module/galay_redis.cppm` |
| [03-使用指南](03-使用指南.md) | 从旧接口迁移到 `command` / `batch` / `execute` | `examples/`、`test/t4~t13` |
| [04-示例代码](04-示例代码.md) | 示例、测试、运行命令、环境变量 | `examples/CMakeLists.txt`、`test/CMakeLists.txt` |
| [05-性能测试](05-性能测试.md) | 当前 benchmark 目标、CLI、结果声明 | `benchmark/*.cc`、`benchmark/CMakeLists.txt` |
| [06-高级主题](06-高级主题.md) | 模块构建、single-flight、限制与权衡 | `galay-redis/CMakeLists.txt`、`test/t12_topology.cc` |
| [07-常见问题](07-常见问题.md) | 迁移、构建、返回值、拓扑排障 | 上述页面与实际源码 |

## 主题边界

- `00` 只负责最短路径，不承载完整 API 细节
- `01` 只讲结构与职责，不替代 API 参考
- `02` 只对齐公开头文件、模块导出面与返回类型，不混入 benchmark 结果
- `03` 只讲实战用法和迁移
- `04` 只做示例/测试/命令映射
- `05` 只做 benchmark 说明与结果声明
- `06` 只放高级能力与限制
- `07` 只收录高频问题

## 额外索引

- 仓库总览：[README.md](../README.md)
