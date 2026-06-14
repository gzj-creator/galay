# 02-API参考

> 本页以 `galay-rpc/protoc/*.h`、`galay-rpc/kernel/*.h`、`galay-rpc/module/galay_rpc.cppm` 的当前公开签名为准；若示例与本页冲突，以头文件和模块接口文件为真。

## 公开头文件与模块入口

- 协议层：
  - `galay-rpc/protoc/rpc_base.h`
  - `galay-rpc/protoc/rpc_message.h`
  - `galay-rpc/protoc/rpc_codec.h`
  - `galay-rpc/protoc/rpc_error.h`
- 内核层：
  - `galay-rpc/kernel/rpc_service.h`
  - `galay-rpc/kernel/rpc_await.h`
  - `galay-rpc/kernel/rpc_conn.h`
  - `galay-rpc/kernel/rpc_server.h`
  - `galay-rpc/kernel/rpc_client.h`
  - `galay-rpc/kernel/rpc_stream.h`
  - `galay-rpc/kernel/streamsvc.h`
  - `galay-rpc/kernel/rpc_discovery.h`
- 模块入口：
  - `galay-rpc/module/module_prelude.hpp`
  - `galay-rpc/module/galay_rpc.cppm`

安装导出与模块边界：

- header 模式 target：`galay-rpc::galay-rpc`
- 条件模块 target：`galay-rpc::galay-rpc-modules`
- module 名：`galay.rpc`

## 目录

1. [协议层 (protoc/)](#协议层)
   - [rpc_base.h - 基础定义](#rpcbaseh)
   - [rpc_message.h - 消息定义](#rpcmessageh)
   - [rpc_codec.h - 编解码器](#rpccodech)
   - [rpc_error.h - 错误处理](#rpcerrorh)
2. [内核层 (kernel/)](#内核层)
   - [rpc_service.h - 服务定义](#rpcserviceh)
   - [rpc_await.h - Awaitable 基类](#rpcawaith)
   - [rpc_conn.h - 连接封装](#rpcconnh)
   - [rpc_server.h - 服务器](#rpcserverh)
   - [rpc_client.h - 客户端](#rpcclienth)
   - [rpc_stream.h - 双向流](#rpcstreamh)
   - [streamsvc.h - 流式服务器](#streamsvch)
   - [rpc_discovery.h - 服务发现](#rpcdiscoveryh)

---

> 本页以 `galay-rpc/kernel/*.h` 与 `galay-rpc/protoc/*.h` 当前公开签名为准；若示例与本页冲突，以头文件为准。

---

## 协议层

### rpc_base.h

基础常量、字节序辅助、调用模式与 flags 编解码定义。

#### 常量与字节序辅助

```cpp
namespace galay::rpc {

constexpr uint32_t RPC_MAGIC = 0x47525043;               // 协议魔数 "GRPC"
constexpr uint8_t RPC_VERSION = 0x01;                    // 协议版本
constexpr size_t RPC_HEADER_SIZE = 16;                   // 固定消息头长度
constexpr size_t RPC_MAX_BODY_SIZE = 16 * 1024 * 1024;  // 最大消息体 16 MB

uint32_t rpcBswap32(uint32_t v);
uint16_t rpcBswap16(uint16_t v);
uint32_t rpcHtonl(uint32_t host);
uint32_t rpcNtohl(uint32_t net);
uint16_t rpcHtons(uint16_t host);
uint16_t rpcNtohs(uint16_t net);

}
```

这些函数全部是头文件内联实现；RAG 检索时优先使用这些真实签名，而不要复述旧文档中的“自动转换”概述。

#### RpcMessageType

消息类型枚举。

```cpp
enum class RpcMessageType : uint8_t {
    REQUEST         = 0x01,   // 普通请求
    RESPONSE        = 0x02,   // 普通响应
    HEARTBEAT       = 0x03,   // 心跳（预留）
    ERROR           = 0x04,   // 错误（预留）
    STREAM_INIT     = 0x10,   // 流初始化请求
    STREAM_INIT_ACK = 0x11,   // 流初始化确认
    STREAM_DATA     = 0x12,   // 流数据
    STREAM_END      = 0x13,   // 流结束
    STREAM_CANCEL   = 0x14,   // 流取消
};
```

#### RpcCallMode

```cpp
enum class RpcCallMode : uint8_t {
    UNARY = 0,
    CLIENT_STREAMING = 1,
    SERVER_STREAMING = 2,
    BIDI_STREAMING = 3,
};
```

#### Flags helpers

```cpp
constexpr uint8_t RPC_FLAG_MODE_MASK = 0x03;
constexpr uint8_t RPC_FLAG_END_STREAM = 0x04;

uint8_t rpcEncodeFlags(RpcCallMode call_mode, bool end_of_stream);
RpcCallMode rpcDecodeCallMode(uint8_t flags);
bool rpcIsEndStream(uint8_t flags);
uint8_t rpcSetEndStreamFlag(uint8_t flags, bool end_of_stream);
```

`RpcHeader::m_flags` 的低 2 位编码 `RpcCallMode`，第 3 位编码 `END_STREAM`。

#### RpcErrorCode

错误码枚举。

```cpp
enum class RpcErrorCode : uint16_t {
    OK                    = 0,    // 成功
    UNKNOWN_ERROR         = 1,    // 未知错误
    SERVICE_NOT_FOUND     = 2,    // 服务未找到
    METHOD_NOT_FOUND      = 3,    // 方法未找到
    INVALID_REQUEST       = 4,    // 无效请求
    INVALID_RESPONSE      = 5,    // 无效响应
    REQUEST_TIMEOUT       = 6,    // 请求超时
    CONNECTION_CLOSED     = 7,    // 连接关闭
    SERIALIZATION_ERROR   = 8,    // 序列化错误
    DESERIALIZATION_ERROR = 9,    // 反序列化错误
    INTERNAL_ERROR        = 10,   // 内部错误
};
```

#### rpcErrorCodeToString

```cpp
const char* rpcErrorCodeToString(RpcErrorCode code);
```

将错误码转换为可读字符串。

---

### rpc_message.h

消息类定义，包含消息头、请求和响应的序列化/反序列化。

#### RpcPayloadView

```cpp
struct RpcPayloadView {
    const char* segment1 = nullptr;
    size_t segment1_len = 0;
    const char* segment2 = nullptr;
    size_t segment2_len = 0;

    size_t size() const;
    bool empty() const;
};
```

这是零拷贝 payload 视图：不拥有内存，只借用外部 buffer。`RpcRequest::payloadView()` / `RpcResponse::payloadView()` 也遵循同样语义。

#### RpcHeader

16 字节固定消息头。

```cpp
struct RpcHeader {
    uint32_t m_magic = RPC_MAGIC;
    uint8_t m_version = RPC_VERSION;
    uint8_t m_type = 0;
    uint8_t m_flags = 0;
    uint8_t m_reserved = 0;
    uint32_t m_request_id = 0;
    uint32_t m_body_length = 0;

    void serialize(char* buffer) const;
    bool deserialize(const char* buffer);
};
```

**注意：** 所有多字节字段使用网络字节序（大端），内部自动转换。

#### RpcRequest

RPC 请求消息。

```cpp
class RpcRequest {
public:
    RpcRequest();
    RpcRequest(uint32_t request_id, std::string_view service, std::string_view method);

    uint32_t requestId() const;
    void requestId(uint32_t id);
    RpcCallMode callMode() const;
    void callMode(RpcCallMode mode);
    bool endOfStream() const;
    void endOfStream(bool end);

    const std::string& serviceName() const;
    void serviceName(std::string_view name);
    void serviceName(std::string&& name);

    const std::string& methodName() const;
    void methodName(std::string_view name);
    void methodName(std::string&& name);

    const std::vector<char>& payload() const;
    size_t payloadSize() const;
    RpcPayloadView payloadView() const;
    void payload(const char* data, size_t len);
    void payload(std::vector<char>&& data);
    void payloadView(const RpcPayloadView& view);

    std::vector<char> serialize() const;
    bool deserializeBody(const char* body, size_t length);
};
```

`payload()` 在借用模式下会按需 materialize 到内部 `std::vector<char>`；若只需要长度或双段 buffer，请优先使用 `payloadSize()` / `payloadView()`。

#### RpcResponse

RPC 响应消息。

```cpp
class RpcResponse {
public:
    RpcResponse();
    RpcResponse(uint32_t request_id, RpcErrorCode error_code = RpcErrorCode::OK);

    uint32_t requestId() const;
    void requestId(uint32_t id);
    RpcCallMode callMode() const;
    void callMode(RpcCallMode mode);
    bool endOfStream() const;
    void endOfStream(bool end);

    RpcErrorCode errorCode() const;
    void errorCode(RpcErrorCode code);

    const std::vector<char>& payload() const;
    size_t payloadSize() const;
    RpcPayloadView payloadView() const;
    void payload(const char* data, size_t len);
    void payload(std::vector<char>&& data);
    void payloadView(const RpcPayloadView& view);

    bool isOk() const;
    std::vector<char> serialize() const;
    bool deserializeBody(const char* body, size_t length);
};
```

---

### rpc_codec.h

编解码器，提供静态方法解码完整消息。

#### DecodeResult

```cpp
enum class DecodeResult {
    COMPLETE,    // 解码完成
    INCOMPLETE,  // 数据不完整，需要更多数据
    ERROR,       // 解码错误
};
```

#### RpcCodec

```cpp
class RpcCodec {
public:
    // 解码消息头
    static DecodeResult decodeHeader(const char* data, size_t length, RpcHeader& header);

    // 解码请求消息（含头部）
    static std::expected<RpcRequest, RpcError> decodeRequest(const char* data, size_t length);

    // 解码响应消息（含头部）
    static std::expected<RpcResponse, RpcError> decodeResponse(const char* data, size_t length);

    // 计算完整消息长度，数据不足返回 0
    static size_t messageLength(const char* data, size_t length);
};
```

---

### rpc_error.h

错误类封装。

#### RpcError

```cpp
class RpcError {
public:
    RpcError();
    RpcError(RpcErrorCode code);
    RpcError(RpcErrorCode code, std::string_view message);

    RpcErrorCode code() const;
    const std::string& message() const;
    bool isOk() const;

    explicit operator bool() const;  // true 表示有错误
    static RpcError from(const kernel::IOError& io_error,
                         RpcErrorCode default_code = RpcErrorCode::INTERNAL_ERROR);
    std::string toString() const;
};
```

- `RpcError::from(...)` 会把底层 `kernel::IOError` 归一化成 RPC 公开错误：
  - 包含 `kernel::kTimeout` 时映射为 `RpcErrorCode::REQUEST_TIMEOUT`
  - 包含 `kernel::kDisconnectError` 时映射为 `RpcErrorCode::CONNECTION_CLOSED`
  - 其他情况使用 `default_code`

---

## 内核层

### rpc_service.h

服务定义和上下文。来源：`galay-rpc/kernel/rpc_service.h`

#### RpcMethodHandler

```cpp
using RpcMethodHandler = std::function<kernel::Coroutine(RpcContext&)>;
using RpcStreamHandler = std::function<kernel::Coroutine(RpcStream&)>;
```

RPC 方法处理函数类型，接收 `RpcContext` 引用，返回协程。

#### RpcService

服务基类，所有 RPC 服务需继承此类。

```cpp
class RpcService {
public:
    explicit RpcService(std::string_view name);
    virtual ~RpcService() = default;

    // 获取服务名称
    const std::string& name() const;

    // 查找方法处理器，未找到返回 nullptr
    RpcMethodHandler* findMethod(const std::string& method);
    RpcMethodHandler* findMethod(const std::string& method, RpcCallMode mode);

    // 查找真实流方法（STREAM_* 会话）
    RpcStreamHandler* findStreamMethod(const std::string& method);

    // 获取所有方法名
    std::vector<std::string> methodNames() const;

protected:
    // 兼容旧接口：注册一元方法
    void registerMethod(std::string_view name, RpcMethodHandler handler);
    template<typename T>
    void registerMethod(std::string_view name, Coroutine (T::*method)(RpcContext&));

    // 显式注册四种 RPC 模式
    void registerUnaryMethod(std::string_view name, RpcMethodHandler handler);
    void registerClientStreamingMethod(std::string_view name, RpcMethodHandler handler);
    void registerServerStreamingMethod(std::string_view name, RpcMethodHandler handler);
    void registerBidiStreamingMethod(std::string_view name, RpcMethodHandler handler);
    template<typename T>
    void registerUnaryMethod(std::string_view name, Coroutine (T::*method)(RpcContext&));
    template<typename T>
    void registerClientStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&));
    template<typename T>
    void registerServerStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&));
    template<typename T>
    void registerBidiStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&));

    // 注册真实流方法（STREAM_* 协议）
    void registerStreamMethod(std::string_view name, RpcStreamHandler handler);
    template<typename T>
    void registerStreamMethod(std::string_view name, Coroutine (T::*method)(RpcStream&));
};
```

补充说明：

- `registerMethod(...)` / `registerMethod<T>(...)` 是 `unary` 模式的兼容别名。
- `findMethod(method, mode)` 会按 `RpcCallMode` 精确选择 `unary` / `client_stream` / `server_stream` / `bidi` 对应的处理器。
- `registerStreamMethod(...)` 用于真实流协议，会把 `STREAM_INIT` 建立出的 `RpcStream` 会话交给业务层处理。
- 头文件里的 `RpcMethodSlots` 是 `RpcService` 私有注册表槽位：内部用 `handlers[] + registered[]` 保存三种 streaming handler；它不是扩展点，也不会单独暴露给业务层。

**使用示例：**

```cpp
class EchoService : public RpcService {
public:
    EchoService() : RpcService("EchoService") {
        registerMethod("echo", &EchoService::echo);
    }

    Coroutine echo(RpcContext& ctx) {
        auto& req = ctx.request();
        ctx.setPayload(req.payload().data(), req.payload().size());
        co_return;
    }
};
```

#### RpcContext

请求上下文，封装请求和响应，传递给服务方法。

```cpp
class RpcContext {
public:
    RpcContext(RpcRequest& request, RpcResponse& response);

    // 获取请求
    RpcRequest& request();
    const RpcRequest& request() const;

    // 获取响应
    RpcResponse& response();
    const RpcResponse& response() const;

    // 设置错误码
    void setError(RpcErrorCode code);

    // 设置响应数据
    void setPayload(const char* data, size_t len);
    void setPayload(const std::string& data);
    void setPayload(std::vector<char>&& data);
    void setPayload(const RpcPayloadView& view);
};
```

---

### rpc_await.h

高级 Awaitable 基类。来源：`galay-rpc/kernel/rpc_await.h`

该头文件已随安装公开面一起导出，主要面向需要扩展自定义读写等待体的高级用户；普通调用方通常通过 `rpc_conn.h` / `rpc_client.h` / `rpc_stream.h` 间接使用它。

#### `detail` 辅助函数

```cpp
namespace detail {
    std::array<struct iovec, 1>& emptyIovecs();
    void consumeWritevIovecs(std::vector<iovec>& iovecs, size_t consumed);
}
```

- `emptyIovecs()`：为 `readv/writev` 基类提供空 `iovec` 占位
- `consumeWritevIovecs(...)`：在部分写完成后推进 `iovec` 视图，支撑可续写等待体

#### `RingBufferReadAwaitable<Derived, SocketType>`

```cpp
template<typename Derived, typename SocketType>
class RingBufferReadAwaitable : public ReadvAwaitable {
public:
    RingBufferReadAwaitable(RingBuffer& ring_buffer, SocketType& socket);

    bool await_ready() const noexcept;

    template<typename Handle>
    auto await_suspend(Handle handle);

    std::expected<bool, RpcError> await_resume();
};
```

- 以 CRTP 方式复用 “先解析 RingBuffer、再按需继续 `readv`” 的共同行为
- `Derived` 需要提供 `parseFromRingBuffer()`，返回：
  - `true`：消息已完整解析
  - `false`：数据仍不足，需要继续异步读取
  - `unexpected(RpcError)`：解析失败
- 内部会把底层 IO 断开转换为 `RpcErrorCode::CONNECTION_CLOSED`

#### `ResumableWriteAwaitable<Derived, SocketType>`

```cpp
template<typename Derived, typename SocketType>
class ResumableWriteAwaitable : public WritevAwaitable {
public:
    explicit ResumableWriteAwaitable(SocketType& socket);

    template<typename Handle>
    auto await_suspend(Handle handle);

    std::expected<bool, RpcError> await_resume();
};
```

- 复用 “部分 `writev` -> 调整 `iovec` -> 继续发送” 的骨架逻辑
- `Derived` 通常在构造阶段填充：
  - `m_iovecs`
  - `m_total_bytes`
- 典型使用者包括请求/响应发送与流数据发送等待体

---

### rpc_conn.h

连接封装，使用 RingBuffer + readv/writev 实现高效 IO。

#### RpcReaderSetting / RpcWriterSetting

```cpp
struct RpcReaderSetting {
    size_t max_message_size = RPC_MAX_BODY_SIZE;  // 最大消息大小
};

struct RpcWriterSetting {
    // 预留扩展
};
```

#### RpcReader (RpcReaderImpl\<TcpSocket\>)

```cpp
class RpcReader {
public:
    RpcReader(RingBuffer& ring_buffer, const RpcReaderSetting& setting, TcpSocket& socket);

    // 获取 RPC 请求（服务端使用）
    // 返回 Awaitable: true=解析完成, false=需要继续读取
    GetRpcRequestAwaitable<TcpSocket> getRequest(RpcRequest& request);

    // 获取 RPC 响应（客户端使用）
    GetRpcResponseAwaitable<TcpSocket> getResponse(RpcResponse& response);

    // 获取消息头（流式传输使用）
    GetRpcHeaderAwaitable<TcpSocket> getHeader(RpcHeader& header);

    // 获取消息体（流式传输使用）
    GetRpcBodyAwaitable<TcpSocket> getBody(char* body, size_t body_len);
};
```

补充公开别名：

- `GetHeaderAwaitable = GetRpcHeaderAwaitable<TcpSocket>`：要求 RingBuffer 至少已有 `RPC_HEADER_SIZE` 字节；头部反序列化失败时返回 `RpcError(INVALID_REQUEST, "Invalid header")`
- `GetBodyAwaitable = GetRpcBodyAwaitable<TcpSocket>`：要求 RingBuffer 至少已有 `body_len` 字节；成功后会消费这段字节并把内容拷贝到调用方提供的缓冲区

#### RpcWriter (RpcWriterImpl\<TcpSocket\>)

```cpp
class RpcWriter {
public:
    RpcWriter(const RpcWriterSetting& setting, TcpSocket& socket);

    // 发送 RPC 请求
    SendRpcRequestAwaitable<TcpSocket> sendRequest(const RpcRequest& request);

    // 发送 RPC 响应
    SendRpcResponseAwaitable<TcpSocket> sendResponse(const RpcResponse& response);

    // 发送原始数据（流式传输使用）
    SendRawDataAwaitable<TcpSocket> sendRaw(const char* data, size_t len);
};
```

补充公开别名：

- `SendRawAwaitable = SendRawDataAwaitable<TcpSocket>`：`sendRaw(const char*, size_t)` 会先把数据复制进内部 `std::vector<char>`；`len == 0` 时 `await_ready()` 直接为 `true`

#### RpcConn (RpcConnImpl\<TcpSocket\>)

```cpp
class RpcConn {
public:
    // 从已有 socket 构造（服务端使用）
    explicit RpcConn(GHandle handle,
                     const RpcReaderSetting& reader_setting = {},
                     const RpcWriterSetting& writer_setting = {});

    // 创建新连接（客户端使用）
    explicit RpcConn(IPType type = IPType::IPV4,
                     const RpcReaderSetting& reader_setting = {},
                     const RpcWriterSetting& writer_setting = {});

    // 连接到服务器
    ConnectAwaitable connect(const Host& host);

    // 获取读取器/写入器
    RpcReader getReader();
    RpcWriter getWriter();

    // 获取底层 socket
    TcpSocket& socket();

    // 关闭连接
    CloseAwaitable close();
};
```

#### Awaitable 返回值约定

所有读写 Awaitable 的返回类型为 `std::expected<bool, RpcError>`：

| 返回值 | 含义 |
|--------|------|
| `true` | 操作完成 |
| `false` | 操作未完成，需要继续 `co_await` |
| `unexpected(RpcError)` | 发生错误 |

**标准使用模式：**

```cpp
// 循环等待完成
while (true) {
    auto result = co_await reader.getRequest(request);
    if (!result) {
        // 错误处理
        break;
    }
    if (result.value()) {
        // 完成
        break;
    }
    // result.value() == false，继续读取
}
```

---

### rpc_server.h

RPC 服务器，内置 Runtime。

#### RpcServerConfig

```cpp
struct RpcServerConfig {
    std::string host = "0.0.0.0";        // 监听地址
    uint16_t port = 9000;                // 监听端口
    int backlog = 128;                   // 监听队列长度
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;       // AUTO=自动，0=禁用
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;  // AUTO=自动，0=禁用
    RuntimeAffinityConfig affinity;      // 绑核配置
    size_t ring_buffer_size = 8192;      // RingBuffer 大小
};
```

`GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 定义在 `galay-kernel/kernel/runtime.h`。这里的默认值与上游 Runtime 对齐：`0` 不是“自动”，而是“禁用对应 scheduler”。

#### RpcServerBuilder

```cpp
class RpcServerBuilder {
public:
    RpcServerBuilder& host(std::string value);
    RpcServerBuilder& port(uint16_t value);
    RpcServerBuilder& backlog(int value);
    RpcServerBuilder& ioSchedulerCount(size_t value);
    RpcServerBuilder& computeSchedulerCount(size_t value);
    RpcServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count);
    bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus);
    RpcServerBuilder& ringBufferSize(size_t value);
    RpcServer build() const;
    RpcServerConfig buildConfig() const;
};
```

#### RpcServer

```cpp
class RpcServer {
public:
    explicit RpcServer(const RpcServerConfig& config);
    ~RpcServer();  // 自动调用 stop()

    // 注册服务（启动前调用）
    void registerService(std::shared_ptr<RpcService> service);

    // 启动服务器（非阻塞，内部启动 Runtime 和 accept 循环）
    void start();

    // 停止服务器
    void stop();

    // 检查是否运行中
    bool isRunning() const;

    // 获取内部 Runtime
    Runtime& runtime();

    // 获取最近一次启动/运行错误
    std::optional<RpcError> lastError() const;
};
```

补充说明：

- 头文件中的 `RouteCacheEntry` 是 `RpcServer` 私有路由缓存项，只用于最近命中优化；它不会出现在注册 / 调用公共接口里，也不应被业务代码持有。

**使用示例：**

```cpp
auto service = std::make_shared<EchoService>();

RpcServerConfig config;
config.host = "0.0.0.0";
config.port = 9000;

RpcServer server(config);
server.registerService(service);
server.start();

// 等待停止信号
while (server.isRunning()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

server.stop();
```

---

### rpc_client.h

RPC 客户端，支持异步调用和超时。

#### RpcClientConfig

```cpp
struct RpcClientConfig {
    RpcReaderSetting reader_setting;
    RpcWriterSetting writer_setting;
    size_t ring_buffer_size = 8192;
};
```

#### RpcClientBuilder

```cpp
class RpcClientBuilder {
public:
    RpcClientBuilder& readerSetting(RpcReaderSetting setting);
    RpcClientBuilder& writerSetting(RpcWriterSetting setting);
    RpcClientBuilder& ringBufferSize(size_t size);
    RpcClient build() const;
    RpcClientConfig buildConfig() const;
};
```

#### RpcClient (RpcClientImpl\<TcpSocket\>)

```cpp
class RpcClient {
public:
    explicit RpcClient(const RpcClientConfig& config = RpcClientConfig());

    // 连接到服务器
    ConnectAwaitable connect(const std::string& host, uint16_t port);

    // 一元调用：返回值对象，可直接链式 .timeout(...)
    RpcCallAwaitable call(const std::string& service, const std::string& method,
                          const char* payload, size_t payload_len);
    RpcCallAwaitable call(const std::string& service, const std::string& method,
                          const std::string& payload);
    RpcCallAwaitable call(const std::string& service, const std::string& method);

    RpcCallAwaitable callWithMode(const std::string& service,
                                  const std::string& method,
                                  RpcCallMode mode,
                                  bool end_of_stream,
                                  const char* payload,
                                  size_t payload_len);

    // 模式化流式 RPC 帧调用
    RpcCallAwaitable callClientStreamFrame(const std::string& service,
                                           const std::string& method,
                                           const char* payload,
                                           size_t payload_len,
                                           bool end_of_stream);
    RpcCallAwaitable callServerStreamRequest(const std::string& service,
                                             const std::string& method,
                                             const char* payload,
                                             size_t payload_len);
    RpcCallAwaitable callBidiStreamFrame(const std::string& service,
                                         const std::string& method,
                                         const char* payload,
                                         size_t payload_len,
                                         bool end_of_stream);

    // 创建真实流会话（STREAM_*）
    std::expected<RpcStream, RpcError> createStream(const std::string& service,
                                                    const std::string& method);
    std::expected<RpcStream, RpcError> createStream(uint32_t stream_id,
                                                    const std::string& service = {},
                                                    const std::string& method = {});

    // 关闭连接
    CloseAwaitable close();

    // 获取读取器/写入器（高级用法）
    RpcReader getReader();
    RpcWriter getWriter();

    // 获取底层 socket 和 RingBuffer（高级扩展/自定义协议桥接）
    TcpSocket& socket();
    RingBuffer& ringBuffer();
};
```

补充说明：

- `call(...)` / `callClientStreamFrame(...)` / `callServerStreamRequest(...)` / `callBidiStreamFrame(...)` 都返回 `RpcCallAwaitable` **值对象**，不是引用。
- 因为返回值是 awaitable 对象，所以可以直接链式调用 `.timeout(std::chrono::milliseconds(...))`。
- `callWithMode(...)` 是底层公开入口：它会显式设置 `RpcCallMode` 与 `end_of_stream` flag，然后构造 `RpcRequest` 并返回同一个 `RpcCallAwaitable`。
- `callClientStreamFrame(...)` 和 `callBidiStreamFrame(...)` 需要显式传入 `end_of_stream`；`callServerStreamRequest(...)` 固定以单请求触发服务端流响应。
- `createStream(...)` 只创建 `RpcStream` 会话对象，不会自动发送 `STREAM_INIT`；必须再显式调用 `sendInit()`。

#### `RecvRpcResponseChainAwaitable<SocketType>`

`rpc_client.h` 中公开的高级响应读取上下文，主要供扩展点、测试和自定义调度桥接使用，不是常规业务层首选入口。

```cpp
template<typename SocketType>
class RecvRpcResponseChainAwaitable : public ReadvIOContext {
public:
    RecvRpcResponseChainAwaitable(RingBuffer& ring_buffer,
                                  const RpcReaderSetting& setting,
                                  uint32_t expected_request_id,
                                  RpcResponse& response);

    const std::expected<void, RpcError>& result() const;
};
```

- 它会持续从 `RingBuffer` + `readv` 链路里读取，直到解析出 `expected_request_id` 对应的完整响应，或返回 `RpcError`。
- 这是 `RpcCallAwaitableImpl` 复用的底层公开类型；普通业务场景优先直接使用 `client.call(...)`。

#### RpcCallAwaitable

RPC 调用等待体，封装发送请求 + 接收响应的完整流程。

```cpp
// 返回类型
using CallResult = std::expected<std::optional<RpcResponse>, RpcError>;
```

| 返回值 | 含义 |
|--------|------|
| `RpcResponse` | 调用完成，包含响应 |
| `std::nullopt` | 操作未完成，需要继续 `co_await` |
| `unexpected(RpcError)` | 发生错误（超时、连接关闭等） |

**使用示例：**

```cpp
Coroutine example(RpcClient& client) {
    // 基本调用
    while (true) {
        auto result = co_await client.call("EchoService", "echo", "Hello");
        if (!result) {
            // 错误: result.error().message()
            break;
        }
        if (result.value()) {
            auto& response = result.value().value();
            // 处理响应
            break;
        }
        // 继续等待
    }

    // 带超时调用（毫秒）
    while (true) {
        auto result = co_await client.call("EchoService", "echo", "Hello")
            .timeout(std::chrono::milliseconds(5000));
        if (!result) {
            if (result.error().code() == RpcErrorCode::REQUEST_TIMEOUT) {
                // 超时处理
            }
            break;
        }
        if (result.value()) {
            break;
        }
    }
}
```

---

### rpc_stream.h

双向流支持。

#### StreamMessage

流消息。

```cpp
class StreamMessage {
public:
    StreamMessage();
    StreamMessage(uint32_t stream_id, const char* data, size_t len);

    uint32_t streamId() const;
    void streamId(uint32_t id);

    const std::vector<char>& payload() const;
    std::string payloadStr() const;
    void payload(const char* data, size_t len);
    void payload(const std::string& data);

    bool isEnd() const;
    void setEnd(bool end = true);

    RpcMessageType messageType() const;
    void messageType(RpcMessageType type);

    std::vector<char> serialize(RpcMessageType type) const;
    bool deserializeBody(const char* body, size_t length);
};
```

#### StreamInitRequest

流初始化请求。

```cpp
class StreamInitRequest {
public:
    StreamInitRequest();
    StreamInitRequest(uint32_t stream_id, std::string_view service, std::string_view method);

    uint32_t streamId() const;
    const std::string& serviceName() const;
    const std::string& methodName() const;

    std::vector<char> serialize() const;
    bool deserializeBody(const char* body, size_t length);
};
```

#### StreamReader (StreamReaderImpl\<TcpSocket\>)

```cpp
class StreamReader {
public:
    StreamReader(RingBuffer& ring_buffer, TcpSocket& socket);

    // 获取流消息
    // 返回 Awaitable: true=接收完成, false=需要继续
    GetStreamMessageAwaitable<TcpSocket> getMessage(StreamMessage& msg);
};
```

补充公开名词：

- `GetStreamMessageAwaitable<TcpSocket>` 是 `StreamReader::getMessage(...)` 的真实等待体类型
- 其内部私有状态机 `State { ReadHeader, ReadBody }` 只用于跨多次 `co_await` 保留解析进度，不是业务侧可以设置的流模式

#### StreamWriter (StreamWriterImpl\<TcpSocket\>)

```cpp
class StreamWriter {
public:
    StreamWriter(TcpSocket& socket, uint32_t stream_id);

    // 发送流数据
    SendStreamDataAwaitable<TcpSocket> sendData(const char* data, size_t len);
    SendStreamDataAwaitable<TcpSocket> sendData(const std::string& data);

    // 发送流初始化请求
    SendStreamDataAwaitable<TcpSocket> sendInit(const std::string& service, const std::string& method);

    // 发送流初始化确认
    SendStreamDataAwaitable<TcpSocket> sendInitAck();

    // 发送流结束
    SendStreamDataAwaitable<TcpSocket> sendEnd();

    // 发送流取消
    SendStreamDataAwaitable<TcpSocket> sendCancel();
};
```

#### RpcStream (RpcStreamImpl\<TcpSocket\>)

```cpp
class RpcStream {
public:
    RpcStream(TcpSocket& socket, RingBuffer& ring_buffer, uint32_t stream_id,
              std::string service_name = {}, std::string method_name = {});

    uint32_t streamId() const;
    const std::string& serviceName() const;
    const std::string& methodName() const;
    void setRoute(std::string service_name, std::string method_name);

    StreamReader& getReader();
    StreamWriter& getWriter();

    GetStreamMessageAwaitable<TcpSocket> read(StreamMessage& msg);
    SendStreamDataAwaitable<TcpSocket> sendInit();
    SendStreamDataAwaitable<TcpSocket> sendInit(const std::string& service, const std::string& method);
    SendStreamDataAwaitable<TcpSocket> sendInitAck();
    SendStreamDataAwaitable<TcpSocket> sendData(const char* data, size_t len);
    SendStreamDataAwaitable<TcpSocket> sendData(const std::string& data);
    SendStreamDataAwaitable<TcpSocket> sendEnd();
    SendStreamDataAwaitable<TcpSocket> sendCancel();

    TcpSocket& socket();
    RingBuffer& ringBuffer();
};
```

**使用示例：**

```cpp
Coroutine biStreamExample(RpcStream& stream) {
    // 发送数据
    while (true) {
        auto result = co_await stream.sendData("Hello");
        if (!result || result.value()) break;
    }

    // 接收数据
    StreamMessage msg;
    while (true) {
        auto result = co_await stream.read(msg);
        if (!result || result.value()) break;
    }
    // msg.payloadStr() 获取数据

    // 结束流
    while (true) {
        auto result = co_await stream.sendEnd();
        if (!result || result.value()) break;
    }
}
```

### streamsvc.h

流式服务器，按 `STREAM_INIT` 的 `service/method` 路由到 `RpcService::registerStreamMethod`。

```cpp
struct RpcStreamServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 9100;
    int backlog = 1024;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;
    size_t ring_buffer_size = 128 * 1024;
};

class RpcStreamServerBuilder {
public:
    RpcStreamServerBuilder& host(std::string value);
    RpcStreamServerBuilder& port(uint16_t value);
    RpcStreamServerBuilder& backlog(int value);
    RpcStreamServerBuilder& ioSchedulerCount(size_t value);
    RpcStreamServerBuilder& computeSchedulerCount(size_t value);
    RpcStreamServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count);
    bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus);
    RpcStreamServerBuilder& ringBufferSize(size_t value);
    RpcStreamServer build() const;
    RpcStreamServerConfig buildConfig() const;
};

class RpcStreamServer {
public:
    explicit RpcStreamServer(const RpcStreamServerConfig& config);
    ~RpcStreamServer();
    void registerService(std::shared_ptr<RpcService> service);
    void start();
    void stop();
    bool isRunning() const;
    Runtime& runtime();
    std::optional<RpcError> lastError() const;
};
```

---

### rpc_discovery.h

服务发现模块，使用 C++23 Concept 约束。

#### ServiceEndpoint

服务端点信息。

```cpp
struct ServiceEndpoint {
    std::string host;            // 主机地址
    uint16_t port;               // 端口
    std::string service_name;    // 服务名
    std::string instance_id;     // 实例ID
    uint32_t weight = 100;       // 权重

    std::string address() const; // 返回 "host:port"
};
```

#### DiscoveryError

服务发现错误。

```cpp
struct DiscoveryError {
    enum Code {
        OK = 0,
        NOT_FOUND,
        CONNECTION_ERROR,
        LOCK_TIMEOUT,
        INTERNAL_ERROR
    };

    Code code = OK;
    std::string message;

    bool isOk() const;
    explicit operator bool() const;  // true 表示有错误
};
```

#### ServiceEvent / ServiceWatchCallback

```cpp
enum class ServiceEventType { ADDED, REMOVED, UPDATED };

struct ServiceEvent {
    ServiceEventType type;
    ServiceEndpoint endpoint;
};

using ServiceWatchCallback = std::function<void(const ServiceEvent&)>;
```

#### ServiceRegistry Concept

同步服务注册中心接口约束。

```cpp
template<typename T>
concept ServiceRegistry = requires(T registry,
                                   const std::string& service_name,
                                   const ServiceEndpoint& endpoint,
                                   ServiceWatchCallback callback) {
    { registry.registerService(endpoint) }
        -> std::same_as<std::expected<void, DiscoveryError>>;
    { registry.deregisterService(endpoint) }
        -> std::same_as<std::expected<void, DiscoveryError>>;
    { registry.discoverService(service_name) }
        -> std::same_as<std::expected<std::vector<ServiceEndpoint>, DiscoveryError>>;
    { registry.watchService(service_name, callback) }
        -> std::same_as<std::expected<void, DiscoveryError>>;
    { registry.unwatchService(service_name) }
        -> std::same_as<void>;
};
```

#### AsyncServiceRegistry Concept

异步服务注册中心接口约束（协程环境）。

```cpp
template<typename T>
concept AsyncServiceRegistry = requires(T registry,
                                        const std::string& service_name,
                                        const ServiceEndpoint& endpoint,
                                        ServiceWatchCallback callback) {
    { registry.registerServiceAsync(endpoint) }   -> std::same_as<kernel::Coroutine>;
    { registry.deregisterServiceAsync(endpoint) } -> std::same_as<kernel::Coroutine>;
    { registry.discoverServiceAsync(service_name) } -> std::same_as<kernel::Coroutine>;
    { registry.watchServiceAsync(service_name, callback) } -> std::same_as<kernel::Coroutine>;
    { registry.unwatchServiceAsync(service_name) } -> std::same_as<kernel::Coroutine>;
    { registry.lastError() }     -> std::same_as<DiscoveryError>;
    { registry.lastEndpoints() } -> std::same_as<std::vector<ServiceEndpoint>>;
};
```

#### LocalServiceRegistry

同步本地服务注册中心（非线程安全）。

```cpp
class LocalServiceRegistry {
public:
    std::expected<void, DiscoveryError> registerService(const ServiceEndpoint& endpoint);
    std::expected<void, DiscoveryError> deregisterService(const ServiceEndpoint& endpoint);
    std::expected<std::vector<ServiceEndpoint>, DiscoveryError>
        discoverService(const std::string& service_name);
    std::expected<void, DiscoveryError>
        watchService(const std::string& service_name, ServiceWatchCallback callback);
    void unwatchService(const std::string& service_name);
};
```

#### AsyncLocalServiceRegistry

异步本地服务注册中心（AsyncMutex 保护，协程安全）。

```cpp
class AsyncLocalServiceRegistry {
public:
    kernel::Coroutine registerServiceAsync(const ServiceEndpoint& endpoint);
    kernel::Coroutine deregisterServiceAsync(const ServiceEndpoint& endpoint);
    kernel::Coroutine discoverServiceAsync(const std::string& service_name);
    kernel::Coroutine watchServiceAsync(const std::string& service_name,
                                        ServiceWatchCallback callback);
    kernel::Coroutine unwatchServiceAsync(const std::string& service_name);

    DiscoveryError lastError() const;
    std::vector<ServiceEndpoint> lastEndpoints() const;
};
```

**注意：** `AsyncLocalServiceRegistry` 返回 `Coroutine`（即 `Awaitable&`），同一实例的同一方法不能被多个调用者并发 `co_await`。多调用者场景应使用独立实例。

#### 负载均衡选择器

直接复用 galay-kernel 的负载均衡器：

```cpp
using RoundRobinSelector       = details::RoundRobinLoadBalancer<ServiceEndpoint>;
using RandomSelector            = details::RandomLoadBalancer<ServiceEndpoint>;
using WeightedRoundRobinSelector = details::WeightRoundRobinLoadBalancer<ServiceEndpoint>;
using WeightedRandomSelector    = details::WeightedRandomLoadBalancer<ServiceEndpoint>;
```

#### ServiceDiscoveryClient

服务发现客户端，组合注册中心和负载均衡。

```cpp
template<ServiceRegistry Registry, typename Selector = RoundRobinSelector>
class ServiceDiscoveryClient {
public:
    explicit ServiceDiscoveryClient(Registry& registry);

    // 获取服务实例（通过负载均衡选择）
    std::expected<ServiceEndpoint, DiscoveryError>
        getServiceEndpoint(const std::string& service_name);

    // 监听服务变更
    std::expected<void, DiscoveryError>
        watch(const std::string& service_name, ServiceWatchCallback callback);

    // 取消监听
    void unwatch(const std::string& service_name);
};
```

**使用示例：**

```cpp
LocalServiceRegistry registry;

// 注册服务
ServiceEndpoint ep;
ep.service_name = "EchoService";
ep.host = "127.0.0.1";
ep.port = 9000;
ep.instance_id = "instance-1";
ep.weight = 100;
registry.registerService(ep);

// 使用轮询负载均衡
ServiceDiscoveryClient<LocalServiceRegistry, RoundRobinSelector> client(registry);

auto result = client.getServiceEndpoint("EchoService");
if (result) {
    auto& selected = result.value();
    // selected.host, selected.port
}

// 使用加权随机负载均衡
ServiceDiscoveryClient<LocalServiceRegistry, WeightedRandomSelector> client2(registry);
```

## 统一返回、协程与生命周期语义

- 协议层与服务发现层的同步入口主要返回 `std::expected<..., RpcError>` 或 `std::expected<..., DiscoveryError>`
- client / conn / stream 相关异步入口主要返回 coroutine / awaitable；真实调用顺序以 `examples/` 与 `test/run_*_smoke.sh` 为准
- `RpcService` / `RpcServer` / `RpcClient` / `RpcStream` 是状态型对象；同一实例的最近一次交互状态不应被多个调用方隐式共享推理
- `AsyncLocalServiceRegistry` 已在上文明确：同一实例的同一方法不应被多个调用者并发 `co_await`

## 交叉验证入口

- include 示例：`examples/include/e1_echo.cc`、`examples/include/e2_echo.cc`、`examples/include/e3_stream.cc`、`examples/include/e4_stream.cc`
- import 示例：`examples/import/e1_echo.cc`、`examples/import/e2_echo.cc`、`examples/import/e3_stream.cc`、`examples/import/e4_stream.cc`
- 协议测试：`test/t1_proto.cpp`
- server / client 测试：`test/t2_server.cpp`、`test/t3_client.cpp`
- smoke 脚本：`test/run_rpc_integration_smoke.sh`、`test/run_echo_example_smoke.sh`、`test/run_stream_example_smoke.sh`
