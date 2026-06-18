# 02-API参考

## 1. 命名空间与头文件

- 命名空间：`galay::mongo`
- 常用 include：
  - `galay-mongo/base/mongo_config.h`
  - `galay-mongo/base/mongo_error.h`
  - `galay-mongo/base/mongo_value.h`
  - `galay-mongo/sync/mongo_client.h`
  - `galay-mongo/async/client.h`

## 2. 基础类型

### 2.1 MongoConfig

路径：`galay-mongo/base/mongo_config.h`

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `host` | `std::string` | `127.0.0.1` | Mongo 地址 |
| `port` | `uint16_t` | `27017` | Mongo 端口 |
| `username` | `std::string` | 空 | 用户名 |
| `password` | `std::string` | 空 | 密码 |
| `database` | `std::string` | `admin` | 默认业务库 |
| `auth_database` | `std::string` | `admin` | SCRAM 认证库 |
| `hello_database` | `std::string` | `admin` | hello 握手使用的数据库 |
| `app_name` | `std::string` | `galay-mongo` | hello 元数据 app 名 |
| `tcp_nodelay` | `bool` | `true` | 是否启用 `TCP_NODELAY` |
| `connect_timeout_ms` | `uint32_t` | `5000` | 连接超时 |
| `recv_buffer_size` | `size_t` | `16384` | 同步连接 RingBuffer 容量（超大单包自动走分段直读 bridge） |

工厂方法：

- `MongoConfig::defaultConfig()`
- `MongoConfig::create(host, port, database="admin")`

### 2.2 MongoError / MongoErrorType

路径：`galay-mongo/base/mongo_error.h`

常见错误类型：

- `MONGO_ERROR_CONNECTION`
- `MONGO_ERROR_AUTH`
- `MONGO_ERROR_PROTOCOL`
- `MONGO_ERROR_TIMEOUT`
- `MONGO_ERROR_SEND`
- `MONGO_ERROR_RECV`
- `MONGO_ERROR_CONNECTION_CLOSED`
- `MONGO_ERROR_SERVER`
- `MONGO_ERROR_INVALID_PARAM`

方法：

- `type()`
- `serverCode()`
- `message()`

### 2.3 MongoValue / MongoDocument / MongoArray / MongoReply

路径：`galay-mongo/base/mongo_value.h`

- `MongoValue`：统一值类型容器。
- `MongoDocument`：键值文档，支持 `append/set/find/at/getXxx`。
- `MongoArray`：数组值，支持 `append/at/values`。
- `MongoReply`：命令响应包装，支持 `ok()/hasCommandError()/errorCode()/errorMessage()`。

## 3. 同步 API（MongoClient）

路径：`galay-mongo/sync/mongo_client.h`

返回类型：

- `MongoResult = std::expected<MongoReply, MongoError>`
- `MongoVoidResult = std::expected<void, MongoError>`

### 3.1 连接与状态

- `MongoVoidResult connect(const MongoConfig& config)`
- `MongoVoidResult connect(const std::string& host, uint16_t port, const std::string& database="admin")`
- `void close()`
- `bool isConnected() const`

### 3.2 通用命令与快捷命令

- `MongoResult command(const std::string& database, const MongoDocument& command)`
- `MongoResult ping(const std::string& database="admin")`

### 3.3 CRUD 快捷接口

- `findOne(database, collection, filter={}, projection={})`
- `insertOne(database, collection, document)`
- `updateOne(database, collection, filter, update, upsert=false)`
- `deleteOne(database, collection, filter)`

### 3.4 认证

- `connect(config)` 内部自动触发认证。
- `username/password` 同时为空：跳过认证。
- 二者仅填其一：返回 `MONGO_ERROR_INVALID_PARAM`。
- 当前机制：`SCRAM-SHA-256`。

## 4. 异步 API（AsyncMongoClient）

路径：`galay-mongo/async/client.h`

### 4.1 构造

- `AsyncMongoClient(IOScheduler* scheduler, AsyncMongoConfig config = AsyncMongoConfig::noTimeout())`

### 4.2 连接

- `MongoConnectAwaitable& connect(MongoConfig config)`
- `MongoConnectAwaitable& connect(std::string_view host, uint16_t port, std::string_view database="admin")`

`co_await` 返回：`std::expected<bool, MongoError>`

- 错误：`!result`
- 完成：`result.value() == true`

### 4.3 命令

- `MongoCommandAwaitable& command(std::string database, MongoDocument command)`
- `MongoCommandAwaitable& ping(std::string database="admin")`
- `MongoPipelineAwaitable& pipeline(std::string database, std::vector<MongoDocument> commands)`

`co_await` 返回：`std::expected<MongoReply, MongoError>`

- 错误：`!result`
- 完成：`result.value()` 为 `MongoReply`

`pipeline` 的 `co_await` 返回：

- `std::expected<std::vector<MongoPipelineResponse>, MongoError>`

其中 `MongoPipelineResponse`：

- `request_id`：请求 id（即发送时的 `requestId`，用于与响应 `responseTo` 对应）
- `reply`：命令成功时有值
- `error`：该条命令失败时有值（例如服务端返回 `ok:0`）

### 4.4 连接与日志控制

- `auto close()`
- `bool isClosed() const`
- 日志入口：`galay::mongo::log::set/get`

说明：日志入口使用 `galay::kernel::BaseLogger`，只影响 `galay-mongo` 自身日志。

### 4.5 AsyncMongoConfig

路径：`galay-mongo/base/mongo_config.h`

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `send_timeout` | `std::chrono::milliseconds` | `-1ms` | `<0` 表示不启用 |
| `recv_timeout` | `std::chrono::milliseconds` | `-1ms` | `<0` 表示不启用 |
| `buffer_size` | `size_t` | `16384` | 环形缓冲区大小 |
| `pipeline_reserve_per_command` | `size_t` | `96` | pipeline 每条命令的编码预留字节估算 |

方法：

- `isSendTimeoutEnabled()`
- `isRecvTimeoutEnabled()`
- `withTimeout(send, recv)`
- `noTimeout()`

## 5. 典型调用片段

### 5.1 Sync

```cpp
#include "galay-mongo/sync/mongo_client.h"
using namespace galay::mongo;

MongoClient s;
MongoConfig cfg;
cfg.host = "127.0.0.1";
cfg.port = 27017;
cfg.database = "admin";

if (auto ok = s.connect(cfg); !ok) {
    // ok.error().message()
}

MongoDocument ping;
ping.append("ping", int32_t(1));
auto rsp = s.command("admin", ping);
```

### 5.2 Async

```cpp
#include "galay-mongo/async/client.h"

Task<void> run(galay::kernel::IOScheduler* sched) {
    galay::mongo::AsyncMongoClient client(sched);
    galay::mongo::MongoConfig cfg;

    std::expected<bool, galay::mongo::MongoError> conn = co_await client.connect(cfg);
    if (!conn) co_return;

    galay::mongo::MongoDocument cmd;
    cmd.append("ping", int32_t(1));

    std::expected<galay::mongo::MongoReply, galay::mongo::MongoError> rsp =
        co_await client.command("admin", std::move(cmd));
    if (!rsp) co_return;

    co_await client.close();
}
```

### 5.3 Async Pipeline（单连接多 in-flight）

```cpp
#include "galay-mongo/async/client.h"

Task<void> run(galay::kernel::IOScheduler* sched) {
    galay::mongo::AsyncMongoClient client(sched);
    galay::mongo::MongoConfig cfg;

    if (auto conn = co_await client.connect(cfg); !conn) co_return;

    std::vector<galay::mongo::MongoDocument> commands;
    galay::mongo::MongoDocument c1;
    c1.append("ping", int32_t(1));
    commands.push_back(std::move(c1));
    galay::mongo::MongoDocument c2;
    c2.append("ping", int32_t(1));
    commands.push_back(std::move(c2));

    auto result = co_await client.pipeline("admin", std::move(commands));
    if (!result) co_return;

    for (const auto& item : *result) {
        if (item.ok()) {
            // item.request_id 对应这条回复
        } else {
            // item.error->message()
        }
    }

    co_await client.close();
}
```
