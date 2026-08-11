# PostgreSQL 模块

`galay-postgres` 是基于 PostgreSQL wire protocol v3 的 C++23 客户端模块，提供同步与异步查询、SCRAM-SHA-256/MD5/明文认证、prepared statement、事务、连接池和 pipeline。

## 阅读顺序

1. [00-快速开始](00-快速开始.md)
2. [01-架构设计](01-架构设计.md)
3. [02-API参考](02-API参考.md)
4. [03-使用指南](03-使用指南.md)
5. [04-示例代码](04-示例代码.md)
6. [05-性能测试](05-性能测试.md)
7. [06-高级主题](06-高级主题.md)
8. [07-常见问题](07-常见问题.md)
9. [08-性能测试报告（2026-08-10）](08-性能测试报告-2026-08-10.md)

## 边界

本模块不链接 `libpq`；TLS、SCRAM-SHA-256-PLUS、COPY、LISTEN/NOTIFY 和 CancelRequest 当前不在实现范围内。协议值默认使用 text format，NULL 由 `std::nullopt` 表示。
