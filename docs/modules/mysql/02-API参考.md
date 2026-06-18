# 02-API参考

本页对齐当前 monorepo 源码树中的 MySQL 公开 include surface：

- `galay-mysql/base/mysql_config.h`
- `galay-mysql/base/mysql_error.h`
- `galay-mysql/base/mysql_log.h`
- `galay-mysql/base/mysql_value.h`
- `galay-mysql/async/client.h`
- `galay-mysql/async/conn_pool.h`
- `galay-mysql/sync/mysql_client.h`
- `galay-mysql/protoc/builder.h`
- `galay-mysql/protoc/mysql_auth.h`
- `galay-mysql/protoc/mysql_packet.h`
- `galay-mysql/protoc/mysql_protocol.h`
- `galay-mysql/module/module_prelude.hpp`

说明：

- 本页覆盖仓库自有、供消费者直接使用的 API。
- `galay-kernel` 的上游类型虽然会出现在签名中，但不在此重复展开。
- `client.h` 中为协程链路暴露的 `Protocol*Awaitable`、生命周期枚举和底层 I/O context 虽然可见，但按实现细节处理；这里记录外部代码应直接依赖的 awaitable / client / protocol API。

## API / 示例 / 测试锚点速查

| 主题 | 公开入口 | 主示例入口 | 主测试 / 验证入口 |
| --- | --- | --- | --- |
| 异步单连接客户端 | `galay-mysql/async/client.h` | `examples/mysql/include/e1_query.cc`、`examples/mysql/include/e5_pipeline.cc` | `test/mysql/t3_client.cc`、`test/mysql/t6_transaction.cc`、`test/mysql/t7_stmt.cc` |
| 异步连接池 | `galay-mysql/async/conn_pool.h` | `examples/mysql/include/e3_pool.cc` | `test/mysql/t5_pool.cc` |
| 同步客户端 | `galay-mysql/sync/mysql_client.h` | `examples/mysql/include/e2_query.cc`、`examples/mysql/include/e4_prepared.cc` | `test/mysql/t4_client.cc` |
| 协议编码 / 认证辅助 | `galay-mysql/protoc/builder.h`、`galay-mysql/protoc/mysql_auth.h`、`galay-mysql/protoc/mysql_protocol.h`、`galay-mysql/protoc/mysql_packet.h` | `examples/mysql/include/e5_pipeline.cc` | `test/mysql/t1_protocol.cc`、`test/mysql/t2_auth.cc` |
| monorepo target | `src/galay-mysql/CMakeLists.txt`、`galay::mysql` | 顶层 `CMakeLists.txt` 的 `GALAY_BUILD_MYSQL` | `test/mysql/CMakeLists.txt` |

- 当 import / module 构建路径启用时，对应示例也存在于 `examples/mysql/import/`。
- 回答“这个 API 在哪有真实消费者”时，应优先回到上表中的 examples / tests，再回到 Markdown 说明。

## 基础结果类型

```cpp
using MysqlResult = std::expected<MysqlResultSet, MysqlError>;
using MysqlVoidResult = std::expected<void, MysqlError>;
using MysqlBatchResult = std::expected<std::vector<MysqlResultSet>, MysqlError>;
```

- `MysqlResult` / `MysqlVoidResult` 同时出现在异步与同步头文件中。
- `MysqlBatchResult` 当前由 `galay-mysql/sync/mysql_client.h` 导出，用于 `batch()` / `pipeline()` 的同步返回值。
- 异步 `await_resume()` 则统一返回 `std::expected<std::optional<T>, MysqlError>`；当前实现中的成功路径都会构造有值 `optional`，示例与测试保留 `has_value()` 检查属于防御式消费方式。

## 配置类型

### `MysqlConfig`

```cpp
struct MysqlConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string username;
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    uint32_t connect_timeout_ms = 5000;

    static MysqlConfig defaultConfig();
    static MysqlConfig create(const std::string& host, uint16_t port,
                              const std::string& user, const std::string& password,
                              const std::string& database = "");
};
```

### `AsyncMysqlConfig`

```cpp
struct AsyncMysqlConfig {
    std::chrono::milliseconds send_timeout = std::chrono::milliseconds(-1);
    std::chrono::milliseconds recv_timeout = std::chrono::milliseconds(-1);
    size_t buffer_size = 16384;
    size_t result_row_reserve_hint = 0;

    bool isSendTimeoutEnabled() const;
    bool isRecvTimeoutEnabled() const;

    static AsyncMysqlConfig withTimeout(std::chrono::milliseconds send,
                                        std::chrono::milliseconds recv);
    static AsyncMysqlConfig withRecvTimeout(std::chrono::milliseconds recv);
    static AsyncMysqlConfig withSendTimeout(std::chrono::milliseconds send);
    static AsyncMysqlConfig noTimeout();
};
```

### `MysqlConnectionPoolConfig`

```cpp
struct MysqlConnectionPoolConfig {
    MysqlConfig mysql_config = MysqlConfig::defaultConfig();
    AsyncMysqlConfig async_config = AsyncMysqlConfig::noTimeout();
    size_t min_connections = 2;
    size_t max_connections = 10;
};
```

- `MysqlConfig::create()` 只覆写 host / port / username / password / database；`charset` 仍保持默认 `utf8mb4`，`connect_timeout_ms` 仍保持默认 `5000`。
- `AsyncMysqlConfig` 中 `< 0ms` 的 `send_timeout` / `recv_timeout` 表示关闭超时。
- `buffer_size` 用于异步客户端内部接收 `RingBuffer` 的容量。
- `result_row_reserve_hint` 仅是异步结果集的 `reserveRows()` 提示，不改变协议语义。

## 错误、字段与结果集类型

### `MysqlError`

```cpp
enum MysqlErrorType {
    MYSQL_ERROR_SUCCESS,
    MYSQL_ERROR_CONNECTION,
    MYSQL_ERROR_AUTH,
    MYSQL_ERROR_QUERY,
    MYSQL_ERROR_PROTOCOL,
    MYSQL_ERROR_TIMEOUT,
    MYSQL_ERROR_SEND,
    MYSQL_ERROR_RECV,
    MYSQL_ERROR_CONNECTION_CLOSED,
    MYSQL_ERROR_PREPARED_STMT,
    MYSQL_ERROR_TRANSACTION,
    MYSQL_ERROR_SERVER,
    MYSQL_ERROR_INTERNAL,
    MYSQL_ERROR_BUFFER_OVERFLOW,
    MYSQL_ERROR_INVALID_PARAM,
};

class MysqlError {
public:
    MysqlError(MysqlErrorType type);
    MysqlError(MysqlErrorType type, std::string extra_msg);
    MysqlError(MysqlErrorType type, uint16_t server_errno, std::string server_msg);

    MysqlErrorType type() const;
    std::string message() const;
    uint16_t serverErrno() const;
};
```

- 当前仓库里的常见归类可以按调用面理解：网络建立失败归到 `MYSQL_ERROR_CONNECTION`，认证失败归到 `MYSQL_ERROR_AUTH`，服务端 `ERR` 包通常归到 `MYSQL_ERROR_SERVER`，报文缺失 / 空 payload / 解析失败归到 `MYSQL_ERROR_PROTOCOL`，上游超时回落为 `MYSQL_ERROR_TIMEOUT`，状态机未到预期终态则归到 `MYSQL_ERROR_INTERNAL`。
- `MysqlError(server_errno, server_msg)` 这一路保留服务端 errno；需要区分“库侧失败”与“服务端拒绝”时，优先先看 `type()`，再看 `serverErrno()` / `message()`。

### `MysqlFieldType` / `MysqlFieldFlags`

```cpp
enum class MysqlFieldType : uint8_t {
    DECIMAL, TINY, SHORT, LONG, FLOAT, DOUBLE, NULL_TYPE, TIMESTAMP,
    LONGLONG, INT24, DATE, TIME, DATETIME, YEAR, NEWDATE, VARCHAR, BIT,
    JSON, NEWDECIMAL, ENUM, SET, TINY_BLOB, MEDIUM_BLOB, LONG_BLOB, BLOB,
    VAR_STRING, STRING, GEOMETRY
};

enum MysqlFieldFlags : uint16_t {
    NOT_NULL_FLAG,
    PRI_KEY_FLAG,
    UNIQUE_KEY_FLAG,
    MULTIPLE_KEY_FLAG,
    BLOB_FLAG,
    UNSIGNED_FLAG,
    ZEROFILL_FLAG,
    BINARY_FLAG,
    ENUM_FLAG,
    AUTO_INCREMENT_FLAG,
    TIMESTAMP_FLAG,
    SET_FLAG,
    NUM_FLAG
};
```

`MysqlFieldType` 与 `MysqlFieldFlags` 的完整取值和协议位定义见 `galay-mysql/base/mysql_value.h`。

### `MysqlField` / `MysqlRow` / `MysqlResultSet`

```cpp
class MysqlField {
public:
    MysqlField();
    MysqlField(std::string name, MysqlFieldType type, uint16_t flags,
               uint32_t column_length, uint8_t decimals);

    const std::string& name() const;
    MysqlFieldType type() const;
    uint16_t flags() const;
    uint32_t columnLength() const;
    uint8_t decimals() const;

    void setCatalog(std::string catalog);
    void setSchema(std::string schema);
    void setTable(std::string table);
    void setOrgTable(std::string org_table);
    void setOrgName(std::string org_name);
    void setCharacterSet(uint16_t cs);

    const std::string& catalog() const;
    const std::string& schema() const;
    const std::string& table() const;
    const std::string& orgTable() const;
    const std::string& orgName() const;
    uint16_t characterSet() const;

    bool isNotNull() const;
    bool isPrimaryKey() const;
    bool isAutoIncrement() const;
    bool isUnsigned() const;
};

class MysqlRow {
public:
    MysqlRow();
    explicit MysqlRow(std::vector<std::optional<std::string>> values);

    size_t size() const;
    bool empty() const;
    const std::optional<std::string>& operator[](size_t index) const;
    const std::optional<std::string>& at(size_t index) const;

    bool isNull(size_t index) const;
    std::string getString(size_t index, const std::string& default_val = "") const;
    int64_t getInt64(size_t index, int64_t default_val = 0) const;
    uint64_t getUint64(size_t index, uint64_t default_val = 0) const;
    double getDouble(size_t index, double default_val = 0.0) const;

    const std::vector<std::optional<std::string>>& values() const;
};

class MysqlResultSet {
public:
    MysqlResultSet();

    void addField(MysqlField field);
    void reserveFields(size_t n);
    size_t fieldCount() const;
    const MysqlField& field(size_t index) const;
    const std::vector<MysqlField>& fields() const;

    void addRow(MysqlRow row);
    void reserveRows(size_t n);
    size_t rowCount() const;
    const MysqlRow& row(size_t index) const;
    const std::vector<MysqlRow>& rows() const;

    int findField(const std::string& name) const;

    void setAffectedRows(uint64_t n);
    void setLastInsertId(uint64_t id);
    void setWarnings(uint16_t w);
    void setStatusFlags(uint16_t f);
    void setInfo(std::string info);

    uint64_t affectedRows() const;
    uint64_t lastInsertId() const;
    uint16_t warnings() const;
    uint16_t statusFlags() const;
    const std::string& info() const;
    bool hasResultSet() const;
};
```

## 日志

### `galay::mysql::log::set/get`

```cpp
namespace galay::mysql::log {
void set(::galay::kernel::BaseLogger::uptr logger);
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
}
```

日志宏同样属于安装公开面：

- `MYSQL_LOG_TRACE`
- `MYSQL_LOG_DEBUG`
- `MYSQL_LOG_INFO`
- `MYSQL_LOG_WARN`
- `MYSQL_LOG_ERROR`
- `MYSQL_LOG_ENABLED`

这些宏只读取 `galay::mysql::log::set()` 注入的库级 logger。未设置 logger 或日志级别低于 `minLevel()` 时，不会执行 `std::format`。

## 缓冲配置

异步客户端使用 `galay::utils::RingBuffer` 作为接收缓冲区。`AsyncMysqlConfig::buffer_size` 控制构造 `AsyncMysqlClient` 时创建的 ring buffer 容量；当前公开 API 不提供自定义接收缓冲注入点。

## `AsyncMysqlClientBuilder`

```cpp
class AsyncMysqlClientBuilder {
public:
    AsyncMysqlClientBuilder& scheduler(IOScheduler* scheduler);
    AsyncMysqlClientBuilder& config(AsyncMysqlConfig config);
    AsyncMysqlClientBuilder& sendTimeout(std::chrono::milliseconds timeout);
    AsyncMysqlClientBuilder& recvTimeout(std::chrono::milliseconds timeout);
    AsyncMysqlClientBuilder& bufferSize(size_t size);
    AsyncMysqlClientBuilder& resultRowReserveHint(size_t hint);

    AsyncMysqlClient build() const;
    AsyncMysqlConfig buildConfig() const;
};
```

- `scheduler()` 对当前仓库是事实上的前置条件：`build()` 只是把内部保存的 `IOScheduler*` 透传给 `AsyncMysqlClient` 构造函数，本仓库不做空指针校验。
- `config()` 会整体替换 builder 内部的 `AsyncMysqlConfig`，随后再调用 `sendTimeout()` / `recvTimeout()` / `bufferSize()` / `resultRowReserveHint()` 会继续在这份配置上增量修改。
- `bufferSize()` 影响 `AsyncMysqlClient` 内部接收 ring buffer 的容量。
- 真实消费入口：`README.md` 的异步最小示例、`examples/mysql/include/e1_query.cc`、`test/mysql/t3_client.cc`。

## `AsyncMysqlClient`

`AsyncMysqlClient` 是 move-only 类型；复制构造和复制赋值被删除。

```cpp
class AsyncMysqlClient {
public:
    AsyncMysqlClient(IOScheduler* scheduler,
                     AsyncMysqlConfig config = AsyncMysqlConfig::noTimeout());
    AsyncMysqlClient(AsyncMysqlClient&& other) noexcept;
    AsyncMysqlClient& operator=(AsyncMysqlClient&& other) noexcept;
    ~AsyncMysqlClient() = default;

    MysqlConnectAwaitable connect(MysqlConfig config);
    MysqlConnectAwaitable connect(std::string_view host, uint16_t port,
                                  std::string_view user, std::string_view password,
                                  std::string_view database = "");

    MysqlQueryAwaitable query(std::string_view sql);
    MysqlPipelineAwaitable batch(std::span<const protocol::MysqlCommandView> commands);
    MysqlPipelineAwaitable pipeline(std::span<const std::string_view> sqls);

    MysqlPrepareAwaitable prepare(std::string_view sql);
    MysqlStmtExecuteAwaitable stmtExecute(uint32_t stmt_id,
                                          std::span<const std::optional<std::string>> params,
                                          std::span<const uint8_t> param_types = {});
    MysqlStmtExecuteAwaitable stmtExecute(uint32_t stmt_id,
                                          std::span<const std::optional<std::string_view>> params,
                                          std::span<const uint8_t> param_types = {});

    MysqlQueryAwaitable beginTransaction();
    MysqlQueryAwaitable commit();
    MysqlQueryAwaitable rollback();

    MysqlQueryAwaitable ping();
    MysqlQueryAwaitable useDatabase(std::string_view database);

    auto close();
    bool isClosed() const;

    TcpSocket& socket();
    galay::utils::RingBuffer& ringBuffer();
    const galay::utils::RingBuffer& ringBuffer() const;
    protocol::MysqlParser& parser();
    protocol::MysqlEncoder& encoder();
    const AsyncMysqlConfig& asyncConfig() const;
    uint32_t serverCapabilities() const;
    void setServerCapabilities(uint32_t caps);
};
```

这些 accessor 已经是安装公开面的一部分，主要用于：

- 注入或观察底层 socket / ring buffer
- 直接访问协议 parser / encoder 做高级扩展
- 读取构造时生效的 `AsyncMysqlConfig`
- 调试 server capability 协商结果

使用约束：

- 从公开头和当前示例 / 测试可以确认：单个 `AsyncMysqlClient` 暴露的是同一套 `TcpSocket`、`RingBuffer`、`MysqlParser`、`MysqlEncoder` 状态，因此 canonical API **不承诺** 对同一实例的并发 `query()` / `prepare()` / `stmtExecute()` / `batch()` / `pipeline()` 调用安全；需要并发时应改用多个 client 或 `MysqlConnectionPool`。
- `query()` / `prepare()` / `stmtExecute()` / `batch()` / `pipeline()` / 事务辅助通常都应在 `connect()` 成功之后调用。
- `stmtExecute()` 的 `param_types` 是可选 span；编码器在 `param_types.size() < params.size()` 时，会把剩余参数按 `MysqlFieldType::VAR_STRING` 编码。
- `beginTransaction()` / `commit()` / `rollback()` / `ping()` / `useDatabase()` 都只是 `query()` 的语法糖；其中 `ping()` 实际发送的是 `SELECT 1`，不是 `COM_PING`。
- `close()` 会先把 `isClosed()` 置为 `true`，然后直接转发到上游 `TcpSocket::close()`；因此消费者应按仓库示例那样使用 `co_await client.close();`。本仓库自身不会在这一步额外发送 MySQL `QUIT` 包。
- `isClosed()` 是本地生命周期标记，不是“服务端仍在线”的探针；连接被服务端断开时，仍应以随后一次 awaitable 的返回值为准。
- 真实消费入口：`examples/mysql/include/e1_query.cc`、`examples/mysql/include/e5_pipeline.cc`、`test/mysql/t3_client.cc`、`test/mysql/t6_transaction.cc`、`test/mysql/t7_stmt.cc`。

## 异步 awaitable 类型

`client.h` 公开的主要 awaitable 类型如下：

```cpp
class MysqlConnectAwaitable {
public:
    MysqlConnectAwaitable(AsyncMysqlClient& client, MysqlConfig config);
    bool await_ready() const noexcept;
    using CustomAwaitable::await_suspend;
    std::expected<std::optional<bool>, MysqlError> await_resume();
};

class MysqlQueryAwaitable {
public:
    MysqlQueryAwaitable(AsyncMysqlClient& client, std::string_view sql);
    bool await_ready() const noexcept;
    using CustomAwaitable::await_suspend;
    std::expected<std::optional<MysqlResultSet>, MysqlError> await_resume();
};

class MysqlPrepareAwaitable {
public:
    struct PrepareResult {
        uint32_t statement_id;
        uint16_t num_columns;
        uint16_t num_params;
        std::vector<MysqlField> param_fields;
        std::vector<MysqlField> column_fields;
    };

    MysqlPrepareAwaitable(AsyncMysqlClient& client, std::string_view sql);
    bool await_ready() const noexcept;
    using CustomAwaitable::await_suspend;
    std::expected<std::optional<PrepareResult>, MysqlError> await_resume();
};

class MysqlStmtExecuteAwaitable {
public:
    MysqlStmtExecuteAwaitable(AsyncMysqlClient& client, std::string encoded_cmd);
    bool await_ready() const noexcept;
    using CustomAwaitable::await_suspend;
    std::expected<std::optional<MysqlResultSet>, MysqlError> await_resume();
};

class MysqlPipelineAwaitable {
public:
    MysqlPipelineAwaitable(AsyncMysqlClient& client,
                           std::span<const protocol::MysqlCommandView> commands);
    bool await_ready() const noexcept;
    using CustomAwaitable::await_suspend;
    std::expected<std::optional<std::vector<MysqlResultSet>>, MysqlError> await_resume();
};
```

### `MysqlConnectAwaitable`

- 来源：`AsyncMysqlClient::connect(config)`、`AsyncMysqlClient::connect(host, port, user, password, database)`
- `await_resume()` 返回 `std::expected<std::optional<bool>, MysqlError>`
- 成功值当前固定为 `std::optional<bool>(true)`；它只表示握手和认证已经完成，不直接返回 capability / 握手细节
- 当前实现支持 `mysql_native_password` 与 `caching_sha2_password` 主路径；如果服务端要求 `Auth switch`，实现会返回 `MYSQL_ERROR_AUTH`
- 连接、握手、认证链路中的超时最终会折叠为 `MYSQL_ERROR_TIMEOUT`；未进入最终完成态就恢复结果时，返回 `MYSQL_ERROR_INTERNAL`

### `MysqlQueryAwaitable`

- 来源：`AsyncMysqlClient::query(...)`、`beginTransaction()`、`commit()`、`rollback()`、`ping()`、`useDatabase(...)`
- `await_resume()` 返回 `std::expected<std::optional<MysqlResultSet>, MysqlError>`
- 成功路径返回带值的 `optional<MysqlResultSet>`；当前仓库没有把空 `optional` 当成“继续轮询”的公共协议
- 服务端 `ERR` 包在能解析错误码时映射到 `MYSQL_ERROR_SERVER`，否则回落到 `MYSQL_ERROR_QUERY`
- 包解析失败、列定义 / 行解析失败、状态机落入非法分支时，会返回 `MYSQL_ERROR_PROTOCOL` 或 `MYSQL_ERROR_INTERNAL`
- 上游 `TimeoutSupport` 触发时，当前实现统一折叠为 `MYSQL_ERROR_TIMEOUT`

### `MysqlPrepareAwaitable`

- 来源：`AsyncMysqlClient::prepare(sql)`
- `await_resume()` 返回 `std::expected<std::optional<PrepareResult>, MysqlError>`
- 成功值 `PrepareResult` 包含 `statement_id`、`num_columns`、`num_params`，以及异步路径额外暴露的 `param_fields` / `column_fields`
- 服务端返回 prepare 失败包时，当前实现映射到 `MYSQL_ERROR_PREPARED_STMT`
- prepare 包或列定义解析失败时，返回 `MYSQL_ERROR_PROTOCOL`；未进入最终完成态就恢复结果时，返回 `MYSQL_ERROR_INTERNAL`

### `MysqlStmtExecuteAwaitable`

- 来源：`AsyncMysqlClient::stmtExecute(...)`
- `await_resume()` 返回 `std::expected<std::optional<MysqlResultSet>, MysqlError>`
- 执行成功时既可能得到 OK 结果，也可能得到完整结果集；两种路径都收敛为 `MysqlResultSet`
- 服务端 `ERR` 包在能解析错误码时映射到 `MYSQL_ERROR_SERVER`，否则回落到 `MYSQL_ERROR_QUERY`
- 列定义 / 文本行解析失败会返回 `MYSQL_ERROR_PROTOCOL`；未进入最终完成态就恢复结果时，返回 `MYSQL_ERROR_INTERNAL`

### `MysqlPipelineAwaitable`

- 来源：`AsyncMysqlClient::batch(std::span<const protocol::MysqlCommandView>)`、`AsyncMysqlClient::pipeline(std::span<const std::string_view>)`
- `await_resume()` 返回 `std::expected<std::optional<std::vector<MysqlResultSet>>, MysqlError>`
- 空命令批次会直接落到成功完成态，并返回空 `vector`
- 如果某个 `MysqlCommandView::encoded` 为空，当前实现会在构造阶段直接转成 `MYSQL_ERROR_PROTOCOL`
- pipeline 的每条结果会按发送顺序依次聚合到 `std::vector<MysqlResultSet>`；它不是像 `mongo` 那样的“单条失败可部分成功”模型，一旦发送 / 接收 / 解析出错，整个 awaitable 失败
- 超时最终折叠为 `MYSQL_ERROR_TIMEOUT`；未进入最终完成态就恢复结果时，返回 `MYSQL_ERROR_INTERNAL`

### 内部边界说明

- `MysqlConnectAwaitable::ProtocolConnectAwaitable`
- `MysqlConnectAwaitable::ProtocolHandshakeRecvAwaitable`
- `MysqlConnectAwaitable::ProtocolAuthSendAwaitable`
- `MysqlConnectAwaitable::ProtocolAuthResultRecvAwaitable`
- `MysqlConnectAwaitable::AuthStage` / `Lifecycle`
- `MysqlQueryAwaitable::ProtocolSendAwaitable` / `ProtocolRecvAwaitable`
- `MysqlQueryAwaitable::Lifecycle` / `State`
- `MysqlPrepareAwaitable::ProtocolSendAwaitable` / `ProtocolRecvAwaitable`
- `MysqlPrepareAwaitable::Lifecycle` / `State`
- `MysqlStmtExecuteAwaitable::ProtocolSendAwaitable` / `ProtocolRecvAwaitable`
- `MysqlStmtExecuteAwaitable::Lifecycle` / `State`
- `MysqlPipelineAwaitable::ProtocolSendAwaitable` / `ProtocolRecvAwaitable`
- `MysqlPipelineAwaitable::Lifecycle` / `State`

这些名字虽然位于公开头，但职责是把发送 / 接收 / 握手状态机接入调度器与缓冲抽象；它们属于实现辅助类型，不建议业务层把它们当作稳定扩展 API 依赖。

使用约束：

- awaitable 是按值返回的一次性对象，不要绑定到引用后重复 `co_await`
- 每个 awaitable 只应执行一次 `co_await`
- 成功路径通常应拿到有值的 `optional`
- 空 `optional` 在当前仓库中被当作异常状态，而不是“继续轮询”的协议
- `MysqlConnectAwaitable::await_resume()` 的成功值只是 `optional<bool>(true)`；它不返回握手详情或 capability 结构，服务端 capability 需通过 client accessor 读取。
- `MysqlQueryAwaitable` / `MysqlStmtExecuteAwaitable` 成功时返回 `MysqlResultSet`；服务端 `ERR` 包会映射为 `MysqlError(MYSQL_ERROR_SERVER, ...)`，协议解析失败 / 空 payload / 状态机异常会映射为 `MYSQL_ERROR_PROTOCOL` 或 `MYSQL_ERROR_INTERNAL`。
- `MysqlPrepareAwaitable` 成功时返回的 `PrepareResult` 包含 `statement_id` / `num_columns` / `num_params`，并且异步路径额外暴露 `param_fields` / `column_fields`。
- `MysqlPipelineAwaitable` 对空命令 span 的行为是“成功但结果为空 vector”；如果某个 `MysqlCommandView::encoded` 为空，则在构造阶段就会变成 `MYSQL_ERROR_PROTOCOL`。
- 上游 `galay-kernel` 的超时在这些 awaitable 的 `await_resume()` 中统一折叠为 `MYSQL_ERROR_TIMEOUT`；非超时但未命中的异常则回落为 `MYSQL_ERROR_INTERNAL`。
- 真实锚点：`examples/mysql/include/e1_query.cc`、`examples/mysql/include/e5_pipeline.cc`、`test/mysql/t3_client.cc`、`test/mysql/t7_stmt.cc`。

## `MysqlConnectionPool`

`MysqlConnectionPool` 自身不可复制。

```cpp
class MysqlConnectionPool {
public:
    MysqlConnectionPool(galay::kernel::IOScheduler* scheduler,
                        MysqlConnectionPoolConfig config = {});
    ~MysqlConnectionPool();

    class AcquireAwaitable {
    public:
        AcquireAwaitable(MysqlConnectionPool& pool);
        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        std::expected<std::optional<AsyncMysqlClient*>, MysqlError> await_resume();
    };

    AcquireAwaitable acquire();
    void release(AsyncMysqlClient* client);
    size_t size() const;
    size_t idleCount() const;
};
```

使用约束：

- 成功 `acquire()` 后必须 `release(client)`
- `AcquireAwaitable` 成功时返回的是裸指针，但生命周期仍由连接池管理
- `MysqlConnectionPool` 当前实现会在 `acquire()` 缺少空闲连接且 `size() < max_connections` 时按需创建新 client；`min_connections` 目前只是配置字段，没有“构造时预热”行为。
- `release(nullptr)` 是 no-op；重复归还同一指针不会被池层去重，因此调用方需要自己保证借还配对。
- 池析构时会销毁其持有的全部 client；已借出的 `AsyncMysqlClient*` 在 pool 销毁后立即失效。
- 池内部只对 idle 队列 / waiter 队列 / 连接计数做同步；拿到的 `AsyncMysqlClient*` 仍然遵守单 client 不承诺并发查询安全的约束。
- 真实消费入口：`examples/mysql/include/e3_pool.cc`、`test/mysql/t5_pool.cc`。

### `MysqlConnectionPool::AcquireAwaitable`

- 来源：`MysqlConnectionPool::acquire()`
- `await_resume()` 返回 `std::expected<std::optional<AsyncMysqlClient*>, MysqlError>`
- `Ready` 路径表示池里已有空闲连接；这时不会挂起，恢复结果后直接返回裸指针
- `Creating` 路径会先创建新 client，再内部包一层 `MysqlConnectAwaitable` 完成连接；如果 connect awaitable 缺失、返回空值，或状态机不一致，当前实现会返回 `MYSQL_ERROR_INTERNAL`
- `Waiting` 路径表示池已满且没有空闲连接；被其他连接 `release()` 唤醒后，会再次尝试从池中取连接
- 成功返回的 `AsyncMysqlClient*` 生命周期仍由连接池拥有，调用方必须在使用后执行 `release(client)`
- `AcquireAwaitable::State` 是内部调度状态，不是推荐业务代码依赖的稳定协议

## `MysqlClient`

`MysqlClient` 也是 move-only 类型。

```cpp
class MysqlClient {
public:
    MysqlClient();
    ~MysqlClient();
    MysqlClient(MysqlClient&& other) noexcept;
    MysqlClient& operator=(MysqlClient&& other) noexcept;

    MysqlVoidResult connect(const MysqlConfig& config);
    MysqlVoidResult connect(const std::string& host, uint16_t port,
                            const std::string& user, const std::string& password,
                            const std::string& database = "");

    MysqlResult query(const std::string& sql);
    MysqlBatchResult batch(std::span<const protocol::MysqlCommandView> commands);
    MysqlBatchResult pipeline(std::span<const std::string_view> sqls);

    struct PrepareResult {
        uint32_t statement_id;
        uint16_t num_columns;
        uint16_t num_params;
    };

    std::expected<PrepareResult, MysqlError> prepare(const std::string& sql);
    MysqlResult stmtExecute(uint32_t stmt_id,
                            const std::vector<std::optional<std::string>>& params,
                            const std::vector<uint8_t>& param_types = {});
    MysqlVoidResult stmtClose(uint32_t stmt_id);

    MysqlVoidResult beginTransaction();
    MysqlVoidResult commit();
    MysqlVoidResult rollback();

    MysqlVoidResult ping();
    MysqlVoidResult useDatabase(const std::string& database);

    void close();
    bool isConnected() const;
};
```

- `MysqlClient` 的查询、批量、预处理、事务与工具命令都要求先 `connect()`；否则底层 `sendAll()` / `sendAllv()` / `recvIntoRingBuffer()` 会直接返回 `MYSQL_ERROR_CONNECTION_CLOSED`。
- `batch()` / `pipeline()` 对空输入返回成功的空 `std::vector<MysqlResultSet>`；如果某条 `MysqlCommandView::encoded` 为空，则返回 `MYSQL_ERROR_PROTOCOL`。
- 同步 `prepare()` 只返回 `{statement_id, num_columns, num_params}`；参数列 / 结果列元数据会被内部读取掉，但不会像异步 `PrepareResult` 那样向调用方暴露。
- 同步 `stmtExecute()` 同样把缺省的 `param_types` 视为“剩余参数按 `MysqlFieldType::VAR_STRING` 编码”。
- `stmtClose()` 只发送 `COM_STMT_CLOSE`，不会等待服务端响应。
- `beginTransaction()` / `commit()` / `rollback()` / `ping()` / `useDatabase()` 都复用同步简单语句请求路径；其中 `ping()` 同样执行 `SELECT 1`。
- `close()` 是 best-effort：若当前已连接，会先尝试发送 `QUIT`，无论发送是否成功都继续关闭 socket。
- 从公开头与实现可见，`MysqlClient` 没有对外声明任何线程同步策略；当前 examples / tests 也只按单线程、串行调用方式使用它。
- 真实消费入口：`examples/mysql/include/e2_query.cc`、`examples/mysql/include/e4_prepared.cc`、`test/mysql/t4_client.cc`。

## `protocol::MysqlCommandView` 与 `MysqlCommandBuilder`

当你需要使用 `batch()` 时，公开协议构建接口来自 `galay-mysql/protoc/builder.h`：

```cpp
enum class MysqlCommandKind : uint8_t {
    Raw,
    Query,
    StmtPrepare,
    InitDb,
    Ping,
    Quit,
    ResetConnection
};

struct MysqlCommandView {
    std::string_view encoded;
    MysqlCommandKind kind = MysqlCommandKind::Raw;
    uint8_t sequence_id = 0;
};

struct MysqlEncodedBatch {
    std::string encoded;
    size_t expected_responses = 0;
};

class MysqlCommandBuilder {
public:
    MysqlCommandBuilder() = default;

    void clear() noexcept;
    void reserve(size_t command_count, size_t encoded_bytes);

    MysqlCommandBuilder& appendQuery(std::string_view sql, uint8_t sequence_id = 0);
    MysqlCommandBuilder& appendStmtPrepare(std::string_view sql, uint8_t sequence_id = 0);
    MysqlCommandBuilder& appendInitDb(std::string_view database, uint8_t sequence_id = 0);
    MysqlCommandBuilder& appendPing(uint8_t sequence_id = 0);
    MysqlCommandBuilder& appendQuit(uint8_t sequence_id = 0);
    MysqlCommandBuilder& appendResetConnection(uint8_t sequence_id = 0);
    MysqlCommandBuilder& appendSimple(CommandType cmd,
                                      std::string_view payload = {},
                                      uint8_t sequence_id = 0,
                                      MysqlCommandKind kind = MysqlCommandKind::Raw);
    MysqlCommandBuilder& appendFast(CommandType cmd,
                                    std::string_view payload,
                                    uint8_t sequence_id = 0,
                                    MysqlCommandKind kind = MysqlCommandKind::Raw);

    std::span<const MysqlCommandView> commands() const;
    size_t size() const noexcept;
    bool empty() const noexcept;
    const std::string& encoded() const noexcept;
    MysqlEncodedBatch build() const;
    MysqlEncodedBatch release();
};
```

- `commands()` 返回的 `std::span<const MysqlCommandView>` 以及其中每个 `MysqlCommandView::encoded` 都引用 builder 内部的 `m_encoded` 缓冲；在继续 `append*()`、`clear()` 或 `release()` 之后，先前取出的 view 不应再继续持有。
- `build()` 会复制当前编码后的字节串；`release()` 会把编码结果 move 出去并清空 builder 内部状态。
- `appendPing()` / `appendQuit()` / `appendResetConnection()` 这类低层命令是协议构建入口，不意味着高层 `AsyncMysqlClient` / `MysqlClient` 一定提供同名 wrapper。
- `appendFast()` 的注释前提是“调用方已自行预留足够容量”；保守用法优先选 `reserve()` + `appendQuery()` / `appendSimple()`。
- 真实锚点：`test/mysql/t1_protocol.cc`、`test/mysql/t4_client.cc`、`examples/mysql/include/e5_pipeline.cc`。

## `protocol::AuthPlugin`

`galay-mysql/protoc/mysql_auth.h` 暴露认证辅助：

```cpp
class AuthPlugin {
public:
    static std::string nativePasswordAuth(const std::string& password, const std::string& salt);
    static std::string cachingSha2Auth(const std::string& password, const std::string& salt);
    static std::expected<std::string, std::string>
    cachingSha2FullAuth(const std::string& password,
                        const std::string& salt,
                        std::string_view pem_public_key);

    static std::string sha1(const std::string& data);
    static std::string sha256(const std::string& data);
    static std::string xorStrings(const std::string& a, const std::string& b);
};
```

- `nativePasswordAuth()` 对应 `mysql_native_password`
- `cachingSha2Auth()` 对应 `caching_sha2_password` fast auth
- `cachingSha2FullAuth()` 对应 `caching_sha2_password` 公钥 full auth 辅助
- `cachingSha2FullAuth()` 的返回类型是 `std::expected<std::string, std::string>`；失败信息直接以字符串错误返回，而不是 `MysqlError`。
- 同步 / 异步 connect 链路都会在服务端请求 `caching_sha2_password` full auth 时复用这个 helper；具体验证看 `test/mysql/t2_auth.cc`。

## `protocol::MysqlPacket` 协议模型

`galay-mysql/protoc/mysql_packet.h` 中公开的协议常量 / 枚举 / 结构包括：

- 常量：`MYSQL_PACKET_HEADER_SIZE`、`MYSQL_MAX_PACKET_SIZE`
- 命令枚举：`CommandType`
- 能力枚举：`CapabilityFlags`
- 服务端状态枚举：`ServerStatusFlags`
- 字符集枚举：`CharacterSet`
- 响应类型枚举：`ResponseType`
- 解析错误枚举：`ParseError`

主要协议结构如下：

```cpp
struct PacketHeader {
    uint32_t length = 0;
    uint8_t sequence_id = 0;
};

struct HandshakeV10 {
    uint8_t protocol_version = 0;
    std::string server_version;
    uint32_t connection_id = 0;
    std::string auth_plugin_data;
    uint32_t capability_flags = 0;
    uint8_t character_set = 0;
    uint16_t status_flags = 0;
    std::string auth_plugin_name;
};

struct HandshakeResponse41 {
    uint32_t capability_flags = 0;
    uint32_t max_packet_size = MYSQL_MAX_PACKET_SIZE;
    uint8_t character_set = CHARSET_UTF8MB4_GENERAL_CI;
    std::string username;
    std::string auth_response;
    std::string database;
    std::string auth_plugin_name;
};

struct OkPacket {
    uint64_t affected_rows = 0;
    uint64_t last_insert_id = 0;
    uint16_t status_flags = 0;
    uint16_t warnings = 0;
    std::string info;
};

struct ErrPacket {
    uint16_t error_code = 0;
    std::string sql_state;
    std::string error_message;
};

struct EofPacket {
    uint16_t warnings = 0;
    uint16_t status_flags = 0;
};

struct ColumnDefinitionPacket {
    std::string catalog;
    std::string schema;
    std::string table;
    std::string org_table;
    std::string name;
    std::string org_name;
    uint16_t character_set = 0;
    uint32_t column_length = 0;
    uint8_t column_type = 0;
    uint16_t flags = 0;
    uint8_t decimals = 0;
};

struct ResultSetPacket {
    uint64_t column_count = 0;
    std::vector<ColumnDefinitionPacket> columns;
    std::vector<std::vector<std::optional<std::string>>> rows;
    uint16_t status_flags = 0;
    uint16_t warnings = 0;
};

struct StmtPrepareOkPacket {
    uint32_t statement_id = 0;
    uint16_t num_columns = 0;
    uint16_t num_params = 0;
    uint16_t warning_count = 0;
    std::vector<ColumnDefinitionPacket> param_defs;
    std::vector<ColumnDefinitionPacket> column_defs;
};
```

## `protocol::MysqlParser` / `protocol::MysqlEncoder`

`galay-mysql/protoc/mysql_protocol.h` 暴露协议 helper、parser 与 encoder：

```cpp
std::expected<uint64_t, ParseError> readLenEncInt(const char* data, size_t len, size_t& consumed);
std::expected<std::string, ParseError> readLenEncString(const char* data, size_t len, size_t& consumed);
std::expected<std::string, ParseError> readNullTermString(const char* data, size_t len, size_t& consumed);

uint16_t readUint16(const char* data);
uint32_t readUint24(const char* data);
uint32_t readUint32(const char* data);
uint64_t readUint64(const char* data);

void writeUint16(std::string& buf, uint16_t val);
void writeUint24(std::string& buf, uint32_t val);
void writeUint32(std::string& buf, uint32_t val);
void writeUint64(std::string& buf, uint64_t val);
void writeLenEncInt(std::string& buf, uint64_t val);
void writeLenEncString(std::string& buf, std::string_view str);

class MysqlParser {
public:
    MysqlParser() = default;

    std::expected<PacketHeader, ParseError> parseHeader(const char* data, size_t len);
    std::expected<HandshakeV10, ParseError> parseHandshake(const char* data, size_t len);
    ResponseType identifyResponse(uint8_t first_byte, uint32_t payload_len);
    std::expected<OkPacket, ParseError> parseOk(const char* data, size_t len, uint32_t capabilities);
    std::expected<ErrPacket, ParseError> parseErr(const char* data, size_t len, uint32_t capabilities);
    std::expected<EofPacket, ParseError> parseEof(const char* data, size_t len);
    std::expected<ColumnDefinitionPacket, ParseError> parseColumnDefinition(const char* data, size_t len);
    std::expected<std::vector<std::optional<std::string>>, ParseError>
    parseTextRow(const char* data, size_t len, size_t column_count);
    std::expected<StmtPrepareOkPacket, ParseError> parseStmtPrepareOk(const char* data, size_t len);

    struct PacketView {
        const char* payload;
        uint32_t payload_len;
        uint8_t sequence_id;
    };

    std::expected<PacketView, ParseError> extractPacket(const char* data, size_t len, size_t& consumed);
};

class MysqlEncoder {
public:
    MysqlEncoder() = default;

    std::string encodeHandshakeResponse(const HandshakeResponse41& resp, uint8_t sequence_id);
    std::string encodeQuery(std::string_view sql, uint8_t sequence_id = 0);
    std::string encodeStmtPrepare(std::string_view sql, uint8_t sequence_id = 0);
    std::string encodeStmtExecute(uint32_t stmt_id,
                                  std::span<const std::optional<std::string>> params,
                                  std::span<const uint8_t> param_types,
                                  uint8_t sequence_id = 0);
    std::string encodeStmtExecute(uint32_t stmt_id,
                                  std::span<const std::optional<std::string_view>> params,
                                  std::span<const uint8_t> param_types,
                                  uint8_t sequence_id = 0);
    std::string encodeStmtClose(uint32_t stmt_id, uint8_t sequence_id = 0);
    std::string encodeQuit(uint8_t sequence_id = 0);
    std::string encodePing(uint8_t sequence_id = 0);
    std::string encodeInitDb(std::string_view database, uint8_t sequence_id = 0);
    std::string encodeResetConnection(uint8_t sequence_id = 0);
};
```

- `MysqlParser::extractPacket()` / `parseHeader()` 等会返回 `ParseError::Incomplete`，同步 / 异步接收路径正是依靠这个信号继续从 ring buffer 补读，而不是立即把它当成协议错误。
- `MysqlEncoder::encodePing()` / `encodeQuit()` / `encodeResetConnection()` 主要服务于低层协议拼包；高层客户端目前只把 `query("SELECT 1")` 暴露为 `ping()`。
- 真实锚点：`test/mysql/t1_protocol.cc`、`test/mysql/t4_client.cc`、`examples/mysql/include/e5_pipeline.cc`。

## `module_prelude.hpp`

`galay-mysql/module/module_prelude.hpp` 是 module facade 使用的聚合头文件，但它不是额外的 MySQL API 命名空间；它的职责是：

- 为过渡期 C++23 module / import 构建集中放置系统头、第三方头和公开 MySQL 头
- 供 `.cppm` 模块接口与 import 示例复用同一组 global module fragment 依赖
- 不额外导出独立的 callable API

## monorepo target

当前 monorepo 内部链接方式为：

```cmake
target_link_libraries(app PRIVATE galay::mysql)
```

当前 `src/galay-mysql/CMakeLists.txt` 定义 `mysql` target 和 `galay::mysql` alias；本页不把旧独立仓库安装包路径作为当前 monorepo 的事实。

## 使用备注

- 同步 `close()` 是普通函数；异步 `close()` 需要 `co_await client.close();`
- `pipeline()` 直接接受 `std::span<const std::string_view>`；`batch()` 接受更底层的 `MysqlCommandView`
- 异步 `stmtExecute()` 同时支持 `std::string` 与 `std::string_view` 参数层
- `AsyncMysqlClient::ringBuffer()` / `parser()` / `encoder()` 等 accessor 已经是公开面，适合高级扩展或测试观察
- import / module 文档里的 target 名应与 `examples/CMakeLists.txt` 真实 target 保持一致，不再使用额外别名
