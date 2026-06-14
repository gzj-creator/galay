# 02-API参考

本页按当前**公开安装头文件**与**模块导出**整理 API。若与其他 Markdown 页面冲突，请按以下顺序判断真相来源：

1. `galay-mcp/common/*.h`
2. `galay-mcp/client/*.h`
3. `galay-mcp/server/*.h`
4. `galay-mcp/module/galay_mcp.cppm`

当前对外头文件包括：

- `galay-mcp/common/mcp_json.h`
- `galay-mcp/common/mcp_base.h`
- `galay-mcp/common/mcp_error.h`
- `galay-mcp/common/schema_builder.h`
- `galay-mcp/common/json_parser.h`
- `galay-mcp/common/protocol_utils.h`
- `galay-mcp/client/stdio_client.h`
- `galay-mcp/client/http_client.h`
- `galay-mcp/server/stdio_server.h`
- `galay-mcp/server/http_server.h`
- `galay-mcp/module/module_prelude.hpp`
- `galay-mcp/module/galay_mcp.cppm`

本页优先回答以下检索问题：公开入口是什么、参数 / 返回值是什么、前置条件与失败路径是什么、线程 / 并发语义是什么、示例与测试锚点在哪里。

## 1. 导出 target、包名与模块名

- CMake package：`find_package(galay-mcp CONFIG REQUIRED)`
- 核心 target：`galay-mcp`
- 模块 target：`galay-mcp-modules`（仅在模块支持开启且成功构建时导出）
- C++23 模块名：`galay.mcp`

最小消费方式：

`galay-mcp` 导出的 CMake target 会通过 `INTERFACE_COMPILE_FEATURES` 发布 `cxx_std_23`，因此**链接该 target 的消费者无需再额外手写** `set(CMAKE_CXX_STANDARD 23)`：

```cmake
find_package(galay-mcp CONFIG REQUIRED)
add_executable(your-target main.cc)
target_link_libraries(your-target PRIVATE galay-mcp)
```

如果安装里包含模块 target，则可改为：

```cmake
add_executable(your-target main.cc)
target_link_libraries(your-target PRIVATE galay-mcp-modules)
```

`galay-mcp-modules` 通过公共链接依赖同样继承这一 C++23 要求。

## 2. JSON、协议常量与基础结构

来源：`galay-mcp/common/mcp_json.h`、`galay-mcp/common/mcp_base.h`

```cpp
using JsonString = std::string;
using JsonElement = simdjson::dom::element;
using JsonObject = simdjson::dom::object;
using JsonArray = simdjson::dom::array;

constexpr const char* MCP_VERSION = "2024-11-05";
constexpr const char* JSONRPC_VERSION = "2.0";

namespace Methods {
    constexpr const char* INITIALIZE = "initialize";
    constexpr const char* INITIALIZED = "notifications/initialized";
    constexpr const char* PING = "ping";
    constexpr const char* TOOLS_LIST = "tools/list";
    constexpr const char* TOOLS_CALL = "tools/call";
    constexpr const char* RESOURCES_LIST = "resources/list";
    constexpr const char* RESOURCES_READ = "resources/read";
    constexpr const char* PROMPTS_LIST = "prompts/list";
    constexpr const char* PROMPTS_GET = "prompts/get";
}

namespace ErrorCodes {
    constexpr int PARSE_ERROR = -32700;
    constexpr int INVALID_REQUEST = -32600;
    constexpr int METHOD_NOT_FOUND = -32601;
    constexpr int INVALID_PARAMS = -32602;
    constexpr int INTERNAL_ERROR = -32603;
    constexpr int SERVER_ERROR_START = -32099;
    constexpr int SERVER_ERROR_END = -32000;
}
```

### `MessageType`

来源：`galay-mcp/common/mcp_base.h`

```cpp
enum class MessageType {
    Request,
    Response,
    Notification,
    Error
};
```

| 枚举值 | 含义 |
| --- | --- |
| `Request` | JSON-RPC 请求，通常携带 `id`、`method`、可选 `params` |
| `Response` | JSON-RPC 成功响应，通常携带 `id` 和 `result` |
| `Notification` | JSON-RPC 通知，没有 `id`，不期待响应 |
| `Error` | JSON-RPC 错误响应，通常携带 `id` 和 `error` |

### `ContentType`

来源：`galay-mcp/common/mcp_base.h`

```cpp
enum class ContentType {
    Text,
    Image,
    Resource
};
```

| 枚举值 | `Content` 的有效字段 |
| --- | --- |
| `Text` | `text` |
| `Image` | `data` + `mimeType` |
| `Resource` | `uri` |

### `mcp_json.h` 公开入口

```cpp
class JsonDocument {
public:
    static std::expected<JsonDocument, McpError> Parse(std::string_view json);
    const JsonElement& Root() const;
    JsonElement& Root();
    std::string_view Raw() const;
};

class JsonWriter {
public:
    void StartObject();
    void EndObject();
    void StartArray();
    void EndArray();
    void Key(const std::string& key);
    void String(const std::string& value);
    void Number(int64_t value);
    void Number(uint64_t value);
    void Number(double value);
    void Bool(bool value);
    void Null();
    void Raw(const std::string& json);
    std::string TakeString();
};

class JsonHelper {
public:
    static bool GetObject(const JsonElement& element, JsonObject& out);
    static bool GetArray(const JsonElement& element, JsonArray& out);
    static bool GetStringValue(const JsonElement& element, std::string& out);
    static bool GetRawJson(const JsonElement& element, std::string& out);

    static bool GetString(const JsonObject& obj, const char* key, std::string& out);
    static bool GetInt64(const JsonObject& obj, const char* key, int64_t& out);
    static bool GetBool(const JsonObject& obj, const char* key, bool& out);
    static bool GetElement(const JsonObject& obj, const char* key, JsonElement& out);
    static bool GetObject(const JsonObject& obj, const char* key, JsonObject& out);
    static bool GetArray(const JsonObject& obj, const char* key, JsonArray& out);

    static const JsonElement& EmptyObject();
};
```

说明：

- `JsonDocument::Parse(...)` 失败时返回 `McpError::parseError(...)`；`Root()` / `Raw()` 暴露的视图都依赖 `JsonDocument` 生命周期。
- `JsonWriter::Raw(...)` 会把调用方提供的 JSON 片段**原样写入**输出，不做合法性校验；只适合拼接已经验证过的 JSON。
- `JsonWriter::TakeString()` 会移动走内部缓冲区；公开 API 没有单独的“清空并继续复用”接口。
- `JsonHelper::EmptyObject()` 返回进程级共享的 `{}` DOM 元素，适合“参数缺省时按空对象处理”的场景。
- `JsonWriter` 里的 `ContextType` / `Context` 是私有嵌套类型，只负责跟踪当前在对象还是数组里、是否需要写逗号、对象键之后是否期待 value；它们不是调用方可见扩展点。

公开数据结构包括：

- `Content`
- `Tool`
- `Resource`
- `PromptArgument`
- `Prompt`
- `ClientInfo`
- `ServerInfo`
- `ServerCapabilities`
- `InitializeParams`
- `InitializeResult`
- `ToolCallParams`
- `ToolCallResult`
- `JsonRpcRequest`
- `JsonRpcResponse`
- `JsonRpcNotification`
- `JsonRpcError`

这些结构都提供 `toJson()`；除 `JsonRpcRequest` / `JsonRpcNotification` 外，多数也提供 `fromJson(const JsonElement&)`。

常用结构的字段要求可按以下速查：

| 类型 | 必需字段 / 成功返回形状 | 可选字段 / 边界 |
| --- | --- | --- |
| `Content` | `type` 必需；`text` / `image` / `resource` 三种分支分别要求 `text`、`data`+`mimeType`、`uri` | 未知 `type` 会返回 `invalidMessage("Unknown content type")` |
| `Tool` | `name`、`description` | `inputSchema` 在 `fromJson` 中是可选原始 JSON |
| `Resource` | `uri`、`name`、`description`、`mimeType` | 无 |
| `PromptArgument` | `name`、`description` | `required` 缺省时为 `false` |
| `Prompt` | `name`、`description` | `arguments` 缺省时为空数组 |
| `ClientInfo` / `ServerInfo` | `name`、`version` | `ServerInfo.capabilities` 在 `fromJson` 中是可选原始 JSON |
| `ServerCapabilities` | 顶层对象 | 只检查 `tools` / `resources` / `prompts` / `logging` 字段是否“存在且非 null”，不解析其内部子字段 |
| `InitializeParams` | `protocolVersion`、`clientInfo` | `capabilities` 缺省时按空对象处理 |
| `InitializeResult` | `protocolVersion`、`serverInfo`、`capabilities` | 无 |
| `ToolCallParams` | `name` | `arguments` 缺省时为空对象 |
| `ToolCallResult` | 顶层对象 | `content` 缺省时为空数组；`isError` 缺省时为 `false` |
| `JsonRpcRequest` | `method` | `jsonrpc` 默认 `"2.0"`；`id`、`params` 可选 |
| `JsonRpcNotification` | `method` | `jsonrpc` 默认 `"2.0"`；`params` 可选 |
| `JsonRpcResponse` | `id` | `result` / `error` 都以原始 JSON 字符串保存 |
| `JsonRpcError` | `code`、`message` | `data` 为可选原始 JSON |

## 3. 错误模型

来源：`galay-mcp/common/mcp_error.h`

### `enum class McpErrorCode`

`McpErrorCode` 当前枚举值包括：

- 成功：`Success`
- 连接：`ConnectionFailed`、`ConnectionClosed`、`ConnectionTimeout`
- 协议：`ProtocolError`、`InvalidMessage`、`InvalidMethod`、`InvalidParams`
- JSON-RPC：`ParseError`、`InvalidRequest`、`MethodNotFound`、`InternalError`
- 工具：`ToolNotFound`、`ToolExecutionFailed`
- 资源：`ResourceNotFound`、`ResourceAccessDenied`
- 提示：`PromptNotFound`
- 生命周期：`InitializationFailed`、`AlreadyInitialized`、`NotInitialized`
- IO：`ReadError`、`WriteError`
- 其他：`Unknown`

### `class McpError`

```cpp
class McpError {
public:
    McpError();
    McpError(McpErrorCode code, const std::string& message);
    McpError(McpErrorCode code, const std::string& message, const std::string& details);

    McpErrorCode code() const;
    const std::string& message() const;
    const std::string& details() const;
    bool isSuccess() const;
    std::string toString() const;
    int toJsonRpcErrorCode() const;

    static McpError success();
    static McpError connectionFailed(const std::string& details = "");
    static McpError connectionClosed(const std::string& details = "");
    static McpError connectionError(const std::string& details = "");
    static McpError protocolError(const std::string& details = "");
    static McpError invalidMessage(const std::string& details = "");
    static McpError invalidMethod(const std::string& method);
    static McpError invalidParams(const std::string& details = "");
    static McpError parseError(const std::string& details = "");
    static McpError invalidRequest(const std::string& details = "");
    static McpError methodNotFound(const std::string& method);
    static McpError internalError(const std::string& details = "");
    static McpError toolNotFound(const std::string& toolName);
    static McpError toolExecutionFailed(const std::string& details = "");
    static McpError toolError(const std::string& details = "");
    static McpError resourceNotFound(const std::string& uri);
    static McpError promptNotFound(const std::string& name);
    static McpError initializationFailed(const std::string& details = "");
    static McpError alreadyInitialized();
    static McpError notInitialized();
    static McpError readError(const std::string& details = "");
    static McpError writeError(const std::string& details = "");
    static McpError unknown(const std::string& details = "");
    static McpError invalidResponse(const std::string& details = "");
    static McpError fromJsonRpcError(int code, const std::string& message, const std::string& details = "");
};
```

同步 API 通常返回 `std::expected<T, McpError>`；HTTP 异步 API 则把结果写回调用方提供的 `std::expected<...>&`。

`toJsonRpcErrorCode()` 的映射边界：

- `ParseError` / `InvalidRequest` / `MethodNotFound` / `InvalidMethod` / `InvalidParams` 会映射到对应 JSON-RPC 标准错误码。
- `ToolExecutionFailed`、`InitializationFailed`、`ReadError`、`WriteError` 以及大多数非标准错误会收敛成 `INTERNAL_ERROR`（`-32603`）。

## 4. `SchemaBuilder` 与 `PromptArgumentBuilder`

来源：`galay-mcp/common/schema_builder.h`

### `SchemaBuilder`

```cpp
class SchemaBuilder {
public:
    SchemaBuilder& addString(const std::string& name, const std::string& description, bool required = false);
    SchemaBuilder& addNumber(const std::string& name, const std::string& description, bool required = false);
    SchemaBuilder& addInteger(const std::string& name, const std::string& description, bool required = false);
    SchemaBuilder& addBoolean(const std::string& name, const std::string& description, bool required = false);
    SchemaBuilder& addArray(const std::string& name, const std::string& description, const std::string& itemType = "string", bool required = false);
    SchemaBuilder& addObject(const std::string& name, const std::string& description, const JsonString& objectSchema, bool required = false);
    SchemaBuilder& addObject(const std::string& name, const std::string& description, const SchemaBuilder& objectSchema, bool required = false);
    SchemaBuilder& addEnum(const std::string& name, const std::string& description, const std::vector<std::string>& enumValues, bool required = false);
    JsonString build() const;
};
```

说明：

- `addArray(...)` 的第三个参数是 `itemType`，默认 `"string"`。
- `addObject(...)` 支持直接传入已有 schema JSON，或传入另一个 `SchemaBuilder`。
- `build()` 生成最终 JSON Schema 字符串。
- 头文件中的 `PropertyKind` / `Property` 是 `SchemaBuilder` 私有实现细节：前者表示属性类别，后者保存每个属性的名称、描述、`required`、数组 item 类型、枚举值和对象 schema；调用方只能通过 `addString` / `addNumber` / `addInteger` / `addBoolean` / `addArray` / `addObject` / `addEnum` 间接生成它们。

### `PromptArgumentBuilder`

```cpp
class PromptArgumentBuilder {
public:
    PromptArgumentBuilder& addArgument(const std::string& name, const std::string& description, bool required = false);
    std::vector<PromptArgument> build() const;
};
```

## 5. `McpJsonParser`

来源：`galay-mcp/common/json_parser.h`

```cpp
struct JsonRpcRequestView {
    std::optional<int64_t> id;
    std::string method;
    JsonElement params;
    bool hasParams = false;
};

struct ParsedJsonRpcRequest {
    JsonDocument document;
    JsonRpcRequestView request;
};

struct JsonRpcResponseView {
    int64_t id = 0;
    JsonElement result;
    JsonElement error;
    bool hasResult = false;
    bool hasError = false;
};

struct ParsedJsonRpcResponse {
    JsonDocument document;
    JsonRpcResponseView response;
};

std::expected<ParsedJsonRpcRequest, McpError> parseJsonRpcRequest(std::string_view body);
std::expected<ParsedJsonRpcResponse, McpError> parseJsonRpcResponse(std::string_view body);
```

生命周期说明：

- `JsonRpcRequestView` / `JsonRpcResponseView` 中的 `JsonElement` 都借用自对应的 `JsonDocument`。
- 不要让 `request.params`、`response.result`、`response.error` 脱离 `ParsedJsonRpcRequest::document` 或 `ParsedJsonRpcResponse::document` 的生命周期。
- `parseJsonRpcRequest(...)` 要求顶层是对象、`method` 必须存在且为字符串、`id` 若存在必须是 `int64`。
- `parseJsonRpcResponse(...)` 要求顶层是对象，且 `id` 必须存在并为 `int64`。

## 6. `McpProtocolUtils`

来源：`galay-mcp/common/protocol_utils.h`

```cpp
namespace protocol {

JsonString buildInitializeResult(const std::string& serverName,
                                 const std::string& serverVersion,
                                 bool hasTools,
                                 bool hasResources,
                                 bool hasPrompts);

JsonRpcResponse makeResultResponse(int64_t id, const JsonString& result);

JsonRpcResponse makeErrorResponse(int64_t id,
                                  int code,
                                  const std::string& message,
                                  const std::string& details = "");

template <typename MapType, typename Extractor>
JsonString buildListResultFromMap(const MapType& map, const char* key, Extractor extractor);

} // namespace protocol
```

说明：

- 这些 helper 是**头文件内联函数 / 模板**，没有单独的 `.cc` 实现文件。
- `buildInitializeResult(...)` 直接生成 `InitializeResult` 对应 JSON。
- `buildListResultFromMap(...)` 用于把工具、资源、提示注册表转成统一的列表响应 JSON。

## 7. `McpStdioServer`

来源：`galay-mcp/server/stdio_server.h`

```cpp
class McpStdioServer {
public:
    using ToolHandler = std::function<std::expected<JsonString, McpError>(const JsonElement&)>;
    using ResourceReader = std::function<std::expected<std::string, McpError>(const std::string&)>;
    using PromptGetter = std::function<std::expected<JsonString, McpError>(const std::string&, const JsonElement&)>;

    McpStdioServer();
    ~McpStdioServer();

    void setServerInfo(const std::string& name, const std::string& version);
    void addTool(const std::string& name, const std::string& description, const JsonString& inputSchema, ToolHandler handler);
    void addResource(const std::string& uri, const std::string& name, const std::string& description, const std::string& mimeType, ResourceReader reader);
    void addPrompt(const std::string& name, const std::string& description, const std::vector<PromptArgument>& arguments, PromptGetter getter);

    void run();
    void stop();
    bool isRunning() const;
};
```

约束：

- 拷贝 / 移动被禁用。
- `run()` 阻塞当前线程，持续从 `stdin` 读取请求并向 `stdout` 写响应。

### 入口、返回与失败语义

| 入口 | 参数 | 成功结果 | 失败 / 边界 |
| --- | --- | --- | --- |
| `setServerInfo(name, version)` | 服务器名、版本号 | `void` | 仅影响后续 `initialize` 响应中的 `serverInfo` |
| `addTool(name, description, inputSchema, handler)` | 工具元数据 + `ToolHandler` | `void` | 同名工具会覆盖已有注册项，并重建 `tools/list` 缓存 |
| `addResource(uri, name, description, mimeType, reader)` | 资源元数据 + `ResourceReader` | `void` | 同 URI 会覆盖已有注册项，并重建 `resources/list` 缓存 |
| `addPrompt(name, description, arguments, getter)` | 提示元数据 + `PromptGetter` | `void` | 同名提示会覆盖已有注册项，并重建 `prompts/list` 缓存 |
| `run()` | 无 | `void`，阻塞循环直到 `stop()` 或 `stdin` EOF | 解析失败会向对端发送 `PARSE_ERROR`；空行会在本地被视为 `invalidMessage("Empty message")` 并跳过 |
| `stop()` | 无 | `void` | 只翻转 `m_running`；不会主动关闭 `stdin/stdout` |
| `isRunning()` | 无 | `bool` | 仅读取原子状态 |

### 已实现的 RPC 行为

- `initialize`：要求请求带 `id` 且 `params` 可解析为 `InitializeParams`；重复初始化返回 `INVALID_REQUEST / Already initialized`。
- `tools/list`、`tools/call`、`resources/list`、`resources/read`、`prompts/list`、`prompts/get`：都要求已初始化，否则返回 `INVALID_REQUEST / Not initialized`。
- `tools/call` / `resources/read` / `prompts/get`：缺失 `params`、`name` 或 `uri` 时返回 `INVALID_PARAMS`；未注册项返回 `METHOD_NOT_FOUND`；handler / reader / getter 返回 `McpError` 时会映射成 JSON-RPC 错误响应。
- `ping`：当前实现**不要求初始化**，直接返回空对象结果。
- 成功初始化后，服务端会在响应之后额外发送一条 `notifications/initialized` 通知。

### 线程与并发语义

- 输出写入通过 `m_outputMutex` 串行化；工具 / 资源 / 提示注册表分别由 `std::shared_mutex` 保护。
- 源码允许在锁保护下并发访问注册表，但仓库没有“运行中热更新注册表”的示例或测试；把这类用法视为**未验证能力**更稳妥。
- 头文件中的 `ToolInfo` / `ResourceInfo` / `PromptInfo` 都是私有注册表条目：分别把公开的 `Tool` / `Resource` / `Prompt` 元数据和对应 handler / reader / getter 绑定在一起，不是业务层需要直接操作的类型。

### 示例与测试锚点

- 最小服务器示例：`examples/common/e1_stdio.inc`
- 服务端回归程序：`test/t2_stdio.cc`
- 双向管道联调脚本：`scripts/S4-RunIntegrationTest.sh`

## 8. `McpStdioClient`

来源：`galay-mcp/client/stdio_client.h`

```cpp
class McpStdioClient {
public:
    McpStdioClient();
    ~McpStdioClient();

    std::expected<void, McpError> initialize(const std::string& clientName, const std::string& clientVersion);
    std::expected<JsonString, McpError> callTool(const std::string& toolName, const JsonString& arguments);
    std::expected<std::vector<Tool>, McpError> listTools();
    std::expected<std::vector<Resource>, McpError> listResources();
    std::expected<std::string, McpError> readResource(const std::string& uri);
    std::expected<std::vector<Prompt>, McpError> listPrompts();
    std::expected<JsonString, McpError> getPrompt(const std::string& name, const JsonString& arguments);
    std::expected<void, McpError> ping();
    void disconnect();

    bool isInitialized() const;
    const ServerInfo& getServerInfo() const;
    const ServerCapabilities& getServerCapabilities() const;
};
```

说明：

- 拷贝 / 移动被禁用。
- 请求 / 响应解析直接依赖 `json_parser.h` 中的解析 helper。

### 入口、前置条件与返回

| 入口 | 参数 | 成功结果 | 失败 / 边界 |
| --- | --- | --- | --- |
| `initialize(clientName, clientVersion)` | 客户端名、版本号 | `void`；缓存 `serverInfo` / `serverCapabilities` | 已初始化时返回 `AlreadyInitialized`；初始化响应无法解析时返回 `InitializationFailed` |
| `callTool(toolName, arguments)` | 工具名、原始 JSON 参数 | 返回 `ToolCallResult.content` 的**第一条文本内容**；若内容为空或第一项不是文本则返回 `{}` | 未初始化返回 `NotInitialized`；服务端 `isError=true` 时返回 `ToolExecutionFailed("Tool returned error")` |
| `listTools()` | 无 | `std::vector<Tool>` | 未初始化返回 `NotInitialized`；缺失 `tools` 字段时返回空数组 |
| `listResources()` | 无 | `std::vector<Resource>` | 未初始化返回 `NotInitialized`；缺失 `resources` 字段时返回空数组 |
| `readResource(uri)` | 资源 URI | 返回 `contents` 数组中的第一条文本内容；没有文本内容时返回空字符串 | 未初始化返回 `NotInitialized` |
| `listPrompts()` | 无 | `std::vector<Prompt>` | 未初始化返回 `NotInitialized`；缺失 `prompts` 字段时返回空数组 |
| `getPrompt(name, arguments)` | 提示名、可选原始 JSON 参数 | 返回服务端 `result` 原始 JSON | 未初始化返回 `NotInitialized` |
| `ping()` | 无 | `void` | 未初始化返回 `NotInitialized` |
| `disconnect()` | 无 | `void` | 只清空本地 `m_initialized` 标志，不发送协议级 `disconnect` 消息 |
| `isInitialized()` / `getServerInfo()` / `getServerCapabilities()` | 无 | 本地缓存状态 / 信息 | 仅反映当前实例缓存，不触发 I/O |

### 生命周期与并发语义

- `initialize(...)` 会先发送 `initialize` 请求，成功后再发送 `notifications/initialized` 通知。
- 传输层采用“一行一条 JSON-RPC 消息”的 `stdin/stdout` 协议；`readMessage()` 会跳过空行并持续读取到第一条非空消息。
- 同一 `McpStdioClient` 实例应视为**串行调用对象**：源码虽然给输入 / 输出分别加锁，但 `sendRequest()` 会在单一输入流上直接消费响应并忽略“不是当前 request id”的消息，这会让并发请求互相吞掉对方响应。

### 示例与测试锚点

- 最小客户端示例：`examples/common/e1_stdio.inc`
- 客户端回归程序：`test/t1_stdio.cc`
- 原始协议 / 双向联调脚本：`scripts/S2-Run.sh`、`scripts/S4-RunIntegrationTest.sh`

## 9. `McpHttpServer`

来源：`galay-mcp/server/http_server.h`

```cpp
class McpHttpServer {
public:
    using ToolHandler = std::function<kernel::Coroutine(const JsonElement&, std::expected<JsonString, McpError>&)>;
    using ResourceReader = std::function<kernel::Coroutine(const std::string&, std::expected<std::string, McpError>&)>;
    using PromptGetter = std::function<kernel::Coroutine(const std::string&, const JsonElement&, std::expected<JsonString, McpError>&)>;

    McpHttpServer(const std::string& host = "0.0.0.0",
                  int port = 8080,
                  size_t ioSchedulers = 8,
                  size_t computeSchedulers = 0);
    ~McpHttpServer();

    void setServerInfo(const std::string& name, const std::string& version);
    void addTool(const std::string& name, const std::string& description, const JsonString& inputSchema, ToolHandler handler);
    void addResource(const std::string& uri, const std::string& name, const std::string& description, const std::string& mimeType, ResourceReader reader);
    void addPrompt(const std::string& name, const std::string& description, const std::vector<PromptArgument>& arguments, PromptGetter getter);

    void start();
    void stop();
    bool isRunning() const;
};
```

约束：

- 拷贝 / 移动被禁用。
- `addTool` / `addResource` / `addPrompt` 必须在 `start()` 前完成。
- 公开头文件直接依赖 `galay-http` 与 `galay-kernel`。

### 入口、返回与失败语义

| 入口 | 参数 | 成功结果 | 失败 / 边界 |
| --- | --- | --- | --- |
| `McpHttpServer(host, port, ioSchedulers, computeSchedulers)` | 监听地址、端口；默认 `0.0.0.0:8080`，HTTP runtime 默认 `io=8`、`compute=0` | 构造实例 | 实际绑定失败由底层 `galay-http` 运行时暴露 |
| `setServerInfo(name, version)` | 服务器名、版本号 | `void` | 影响响应头 `Server` 与 `initialize` 返回体 |
| `addTool(...)` / `addResource(...)` / `addPrompt(...)` | 与 `stdio` 版本同名参数 | `void` | 当前头文件明确标注为非线程安全注册阶段；运行期不要动态添加 |
| `start()` | 无 | `void`，阻塞当前线程并监听 `POST /mcp` | 重复调用时直接返回；内部固定回复 `application/json` 且带 `Connection: keep-alive` |
| `stop()` | 无 | `void` | 只清理 `m_running` 与 `m_initialized` 标志 |
| `isRunning()` | 无 | `bool` | 仅读取原子状态 |

### 已实现的 HTTP / RPC 边界

- 仅注册 `POST /mcp` 路由；README、示例、测试中的 HTTP URL 都以该路径为准。
- `tools/list`、`tools/call`、`resources/list`、`resources/read`、`prompts/list`、`prompts/get` 的参数校验、未注册项错误和 `stdio` 服务端一致。
- `ping` 同样不要求初始化，直接返回空对象结果。
- 当前实现同时维护“连接内初始化状态”与进程级 `m_initialized` 标志：一旦有任意连接成功 `initialize`，后续短连接也会被视为已初始化。仓库没有把这点单独固化成测试契约，因此**兼容性最稳妥的做法仍是每个会话都先发 `initialize`**。
- 与 `stdio` 服务端不同，HTTP 服务端成功初始化后**不会**额外发送 `notifications/initialized`。

### 线程与并发语义

- 头文件明确标注：`addTool` / `addResource` / `addPrompt` 必须在 `start()` 前调用，服务器运行期间不支持动态注册。
- 响应列表（tools/resources/prompts）使用惰性缓存；每次注册只标记缓存脏，首次访问列表时再重建。
- `ToolInfo` / `ResourceInfo` / `PromptInfo` 在 `McpHttpServer` 中同样只是私有注册表条目；它们存在于公开头里，但不属于业务侧协议面 API。

### 示例与测试锚点

- 最小 HTTP 服务端示例：`examples/common/e2_http.inc`
- 服务端回归程序：`test/t4_http.cc`
- HTTP 集成脚本：`scripts/S7-RunHttpIntegrationTest.sh`

## 10. `McpHttpClient`

来源：`galay-mcp/client/http_client.h`

```cpp
class McpHttpClient {
public:
    explicit McpHttpClient(kernel::Runtime& runtime);
    ~McpHttpClient();

    ConnectAwaitable connect(const std::string& url);

    kernel::Coroutine initialize(std::string clientName, std::string clientVersion, std::expected<void, McpError>& result);
    kernel::Coroutine callTool(std::string toolName, JsonString arguments, std::expected<JsonString, McpError>& result);
    kernel::Coroutine listTools(std::expected<std::vector<Tool>, McpError>& result);
    kernel::Coroutine listResources(std::expected<std::vector<Resource>, McpError>& result);
    kernel::Coroutine readResource(std::string uri, std::expected<std::string, McpError>& result);
    kernel::Coroutine listPrompts(std::expected<std::vector<Prompt>, McpError>& result);
    kernel::Coroutine getPrompt(std::string name, JsonString arguments, std::expected<JsonString, McpError>& result);
    kernel::Coroutine ping(std::expected<void, McpError>& result);
    CloseAwaitable disconnect();

    bool isConnected() const;
    bool isInitialized() const;
    const ServerInfo& getServerInfo() const;
    const ServerCapabilities& getServerCapabilities() const;
};
```

调用模型：

- 先创建 `kernel::Runtime`。
- `connect()` / `disconnect()` 返回 awaitable。
- 其余 RPC 通过 `kernel::Coroutine` 把结果写回调用方提供的 `std::expected<...>&`。

### 入口、前置条件与返回

| 入口 | 参数 | 成功结果 | 失败 / 边界 |
| --- | --- | --- | --- |
| `McpHttpClient(runtime)` | `kernel::Runtime&` | 构造实例 | 运行时生命周期需覆盖整个客户端对象 |
| `connect(url)` | 服务端 URL，例如 `http://127.0.0.1:8080/mcp` | `ConnectAwaitable` | 该入口也是唯一的“公开设定 URL”方式；返回类型与底层 `http::HttpClient::connect()` 保持一致；后续 RPC 会复用这里保存的 URL |
| `initialize(clientName, clientVersion, result)` | 客户端名、版本号、结果引用 | `result = {}` 并缓存 `serverInfo` / `serverCapabilities` | 解析初始化响应失败时写入 `InitializationFailed` |
| `callTool(toolName, arguments, result)` | 工具名、原始 JSON 参数、结果引用 | `result` 写入第一条文本内容；无文本时写入 `{}` | 未初始化写入 `NotInitialized`；`isError=true` 时写入 `ToolExecutionFailed("Tool returned error")` |
| `listTools(result)` / `listResources(result)` / `listPrompts(result)` | 结果引用 | 写入相应对象数组 | 未初始化写入 `NotInitialized`；缺失列表字段时写入空数组 |
| `readResource(uri, result)` | URI、结果引用 | 写入第一条文本内容；无文本时为空字符串 | 未初始化写入 `NotInitialized` |
| `getPrompt(name, arguments, result)` | 提示名、可选原始 JSON 参数、结果引用 | 写入服务端 `result` 原始 JSON | 未初始化写入 `NotInitialized` |
| `ping(result)` | 结果引用 | 写入空成功结果 | 未初始化写入 `NotInitialized` |
| `disconnect()` | 无 | `CloseAwaitable` | 先清理本地 `m_initialized` / `m_connected` 标志，再返回与底层 `http::HttpClient::close()` 一致的关闭等待体 |
| `isConnected()` / `isInitialized()` / `getServerInfo()` / `getServerCapabilities()` | 无 | 读取本地状态 / 缓存 | 不触发网络 I/O |

### 生命周期与并发语义

- 实践顺序是：创建 `Runtime` → `co_await connect(url)` → `co_await initialize(...).wait()` → 其余 RPC → `co_await disconnect()`。
- `sendRequest(...)` 在 `m_connected == false` 时会自动重连；HTTP 连接若收到 `Connection: close` 或非 keep-alive 响应，也会把本地连接状态清为 `false`。
- 当前 `isConnected()` 反映的是“最近一次成功 RPC 后的连接状态”；单独 `co_await connect(url)` 不会直接把该标志置为 `true`。
- HTTP 状态码不是 `200 OK` 时会被包装成 `connectionError("HTTP error: <code>")`；JSON-RPC `id` 不匹配时返回 `invalidResponse("Mismatched response id")`。
- 公开头文件和测试都没有给出“同一客户端实例可被多个线程 / 协程并发复用”的保证；如需稳妥，调用方应自行串行化。

### 示例与测试锚点

- 最小 HTTP 客户端示例：`examples/common/e2_http.inc`
- 客户端回归程序：`test/t3_http.cc`
- HTTP 集成脚本：`scripts/S7-RunHttpIntegrationTest.sh`

## 11. 模块导出

来源：`galay-mcp/module/module_prelude.hpp`、`galay-mcp/module/galay_mcp.cppm`

`module_prelude.hpp` 是模块构建使用的兼容前导头：它把标准库、`simdjson`、`galay-http`、`galay-kernel` 与 `galay-mcp` 自身公开头放进全局模块片段，降低混合 include / import 构建的失败概率。

模块 `galay.mcp` 重新导出以下公共头：

- `mcp_error.h`
- `mcp_json.h`
- `mcp_base.h`
- `json_parser.h`
- `schema_builder.h`
- `protocol_utils.h`
- `stdio_client.h`
- `http_client.h`
- `stdio_server.h`
- `http_server.h`

## 12. 相关文档

- [00-快速开始](00-快速开始.md) - 首次构建、安装与运行
- [03-使用指南](03-使用指南.md) - 教程式用法与调试流程
- [04-示例代码](04-示例代码.md) - 真实示例文件、target 与命令
- [07-常见问题](07-常见问题.md) - 当前限制与排障
