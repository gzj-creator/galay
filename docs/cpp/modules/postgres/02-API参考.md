# 02-API参考

## 头文件

```cpp
#include <galay/cpp/galay-postgres/async/client.h>
#include <galay/cpp/galay-postgres/async/conn_pool.h>
#include <galay/cpp/galay-postgres/sync/postgres_client.h>
#include <galay/cpp/galay-postgres/protoc/postgres_protocol.h>
```

## 类型

- `PostgresConfig`：主机、端口、用户、密码、数据库、应用名和连接超时。
- `AsyncPostgresConfig`：读写超时、接收缓冲和结果预留提示。
- `PostgresResultSet`：字段、行、command tag 和 affected rows。
- `PostgresError`：typed error、severity、SQLSTATE 和消息。
- `PostgresClient`：同步 connect/query/prepare/execute/transaction/pipeline。
- `AsyncPostgresClient<>`：对应的 awaitable API。
- `PostgresConnectionPool`、`PostgresPoolLease`：异步连接池与 RAII lease。

公开可恢复错误通过 `std::expected` 返回，不使用异常进行流程控制。
