# Galay C PostgreSQL API

C ABI 位于 `src/c/galay-postgres-c/`，公开函数使用 `galay_status_t` 或 `C_IOResult` 显式返回错误，并以 opaque handle 管理 config、client、result set、statement、pipeline、pool 和 lease。

借用的 field/value/message view 只在所属 handle 或输入 buffer 有效时使用；每个 owning handle 必须调用对应 destroy/release API。错误文本通过 `galay_postgres_get_error(...)` 查询。

协议能力与 C++ 模块一致：wire protocol v3、SCRAM-SHA-256/MD5/明文认证、查询、事务、prepared statement、pipeline 和 pool。TLS、COPY、LISTEN/NOTIFY 与 CancelRequest 当前不在范围内。

## 示例、测试与性能

- `examples/c/postgres/e1_async_query.c`：真实 PostgreSQL async query 示例。
- `test/c/postgres/`：C ABI surface、loopback 和真实实例覆盖。
- `benchmark/c/postgres/b1_query_pressure.c`：真实 PostgreSQL simple query 并发压力基准。
- [05-性能测试.md](./05-性能测试.md)：构建、复现口径和本机实测结果。
