# Galay C MySQL API

## 范围

当前 C MySQL 模块提供一个 test-backed async 子集：

- config/buffer/client lifecycle。
- MySQL packet header/extract/query packet helper。
- `mysql_native_password` 与 `caching_sha2_password` fast/full auth response 和 loopback 认证。
- result-set decode、field/row/value accessor、OK packet affected rows/status accessor。
- async query-result、transaction begin/commit/rollback。
- prepared statement prepare/execute 的基础 COM_STMT_PREPARE/COM_STMT_EXECUTE 路径。
- pipeline query builder 和批量发送/逐个读取响应。
- 简单 C pool lease：按需创建 authenticated client，release 后归还 idle client。

## 所有权

`galay_mysql_result_set_decode`、`galay_mysql_client_query_result_async`、
`galay_mysql_client_stmt_prepare_async`、`galay_mysql_client_pipeline_async` 和
`galay_mysql_pool_acquire_async` 成功后转移返回对象所有权。

释放函数：

- `galay_mysql_result_set_destroy`
- `galay_mysql_stmt_destroy`
- `galay_mysql_pipeline_destroy`
- `galay_mysql_pipeline_result_destroy`
- `galay_mysql_pool_lease_release`
- `galay_mysql_pool_destroy`

`galay_mysql_field_view_t` 和 `galay_mysql_value_view_t` 借用 result-set 内部存储；
只在对应 result-set 销毁前有效。

## 错误

公共 API 使用 `galay_status_t` 或 `C_IOResult` 显式返回错误。协议截断、ERR packet、
非法列定义、非法行值和不支持的 auth plugin 返回 `GALAY_PROTOCOL_ERROR` 或
`GALAY_UNSUPPORTED`，再映射到 `C_IOResultError`。`caching_sha2_password` RSA full auth
依赖构建时启用 SSL/RSA 支持；未启用时该路径显式返回 unsupported。

C++ wrapper 实现没有新增 `try`、`catch` 或 `throw`。

## Async 语义

socket API 通过 kernel C TCP coroutine bridge 执行。调用方必须在 Galay C runtime
coroutine 内调用 async 函数。`timeout_ms < 0` 的 connect 使用 config timeout；
其他 socket I/O 超时直接传给 kernel TCP API。

## 示例与测试

- `examples/c/mysql/e2_result_iteration.c`：离线解码并遍历 result-set。
- `examples/c/mysql/e3_prepared_statement.c`：本地 loopback prepared statement。
- `examples/c/mysql/e4_pool_query.c`：本地 loopback pool lease query。
- `test/c/mysql/t4_result_set_decode.c`：result-set/OK packet accessor。
- `test/c/mysql/t5_auth_loopback.c`：native-password、caching SHA2 fast auth 和 RSA full auth loopback。
- `test/c/mysql/t6_stmt_transaction_pool.c`：stmt、transaction、pipeline、pool lease workflow。

## Deferred

仍保留在 C++ API 或后续补齐：

- 真实 MySQL 多结果集、LOCAL INFILE、server-side cursor。
- binary row result decode for prepared statements with returned columns。
- 等待队列、公平调度和并发安全的完整 C pool parity。
- 面向真实 MySQL server 的长期兼容性矩阵。
