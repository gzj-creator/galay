# Galay C PostgreSQL API

C ABI 位于 `src/c/galay-postgres-c/`，公开函数使用 `galay_status_t` 或 `C_IOResult` 显式返回错误，并以 opaque handle 管理 config、client、result set、statement、pipeline、pool 和 lease。

借用的 field/value/message view 只在所属 handle 或输入 buffer 有效时使用；每个 owning handle 必须调用对应 destroy/release API。错误文本通过 `galay_postgres_get_error(...)` 查询。

协议能力与 C++ 模块一致：wire protocol v3、SCRAM-SHA-256/MD5/明文认证、查询、事务、prepared statement、pipeline 和 pool。TLS、COPY、LISTEN/NOTIFY 与 CancelRequest 当前不在范围内。
