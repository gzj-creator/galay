/**
 * @file protocol.h
 * @brief MCP 2026-07-28 核心协议类型与编解码接口。
 */

#ifndef GALAY_MCP_V2_PROTOCOL_H
#define GALAY_MCP_V2_PROTOCOL_H

#include "../../common/mcp_error.h"
#include "../../common/mcp_json.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace galay::mcp::v2 {

inline constexpr const char* MCP_VERSION = "2026-07-28";
inline constexpr const char* JSONRPC_VERSION = "2.0";

namespace Methods {
inline constexpr const char* SERVER_DISCOVER = "server/discover";
inline constexpr const char* TOOLS_LIST = "tools/list";
inline constexpr const char* TOOLS_CALL = "tools/call";
inline constexpr const char* RESOURCES_LIST = "resources/list";
inline constexpr const char* RESOURCES_TEMPLATES_LIST = "resources/templates/list";
inline constexpr const char* RESOURCES_READ = "resources/read";
inline constexpr const char* PROMPTS_LIST = "prompts/list";
inline constexpr const char* PROMPTS_GET = "prompts/get";
inline constexpr const char* SUBSCRIPTIONS_LISTEN = "subscriptions/listen";
inline constexpr const char* CANCELLED = "notifications/cancelled";
}

namespace NotificationMethods {
inline constexpr const char* SUBSCRIPTIONS_ACKNOWLEDGED =
    "notifications/subscriptions/acknowledged";
inline constexpr const char* TOOLS_LIST_CHANGED = "notifications/tools/list_changed";
inline constexpr const char* RESOURCES_LIST_CHANGED =
    "notifications/resources/list_changed";
inline constexpr const char* RESOURCES_UPDATED = "notifications/resources/updated";
inline constexpr const char* PROMPTS_LIST_CHANGED = "notifications/prompts/list_changed";
}

namespace ErrorCodes {
inline constexpr int PARSE_ERROR = -32700;
inline constexpr int INVALID_REQUEST = -32600;
inline constexpr int METHOD_NOT_FOUND = -32601;
inline constexpr int INVALID_PARAMS = -32602;
inline constexpr int INTERNAL_ERROR = -32603;
inline constexpr int HEADER_MISMATCH = -32020;
inline constexpr int MISSING_REQUIRED_CLIENT_CAPABILITY = -32021;
inline constexpr int UNSUPPORTED_PROTOCOL_VERSION = -32022;
}

using RequestId = std::variant<int64_t, std::string>;

/** @brief 实现身份；name/version 必填，其余字段按规范可选。 */
struct Implementation {
    std::string name;
    std::string version;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> websiteUrl;
    std::string icons;

    std::string to_json() const;
    static std::expected<Implementation, McpError> from_json(const json::Json& element);
};

/** @brief 每个 2026-07-28 请求必须携带的元数据。 */
struct RequestMeta {
    std::string protocolVersion{MCP_VERSION};
    std::string clientCapabilities{"{}"};
    std::optional<Implementation> clientInfo;
    std::optional<std::string> logLevel;
    std::optional<RequestId> progressToken;

    std::string to_json() const;
    static std::expected<RequestMeta, McpError> from_json(const json::Json& element);
};

/** @brief 服务器发现所返回的能力集合。 */
struct ServerCapabilities {
    std::string extensions;
    bool tools = false;
    bool resources = false;
    bool prompts = false;
    bool completions = false;
    bool logging = false;
    bool toolsListChanged = false;
    bool resourcesListChanged = false;
    bool resourceSubscriptions = false;
    bool promptsListChanged = false;

    std::string to_json() const;
    static std::expected<ServerCapabilities, McpError> from_json(const json::Json& element);
};

/** @brief 2026-07-28 工具描述；JSON Schema 以已校验的原始 JSON 保存。 */
struct Tool {
    std::string name;
    std::string inputSchema{"{\"type\":\"object\"}"};
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> outputSchema;
    std::optional<std::string> annotations;
    std::optional<std::string> icons;
    std::optional<std::string> meta;

    std::string to_json() const;
    static std::expected<Tool, McpError> from_json(const json::Json& element);
};

struct Resource {
    std::string uri;
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<std::string> mimeType;
    std::optional<uint64_t> size;
    std::optional<std::string> annotations;
    std::optional<std::string> icons;
    std::optional<std::string> meta;

    std::string to_json() const;
    static std::expected<Resource, McpError> from_json(const json::Json& element);
};

struct PromptArgument {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    bool required = false;

    std::string to_json() const;
    static std::expected<PromptArgument, McpError> from_json(const json::Json& element);
};

struct Prompt {
    std::string name;
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::vector<PromptArgument> arguments;
    std::optional<std::string> icons;
    std::optional<std::string> meta;

    std::string to_json() const;
    static std::expected<Prompt, McpError> from_json(const json::Json& element);
};

enum class CacheScope {
    Public,
    Private
};

enum class ResultType {
    Complete,
    InputRequired,
    Extension
};

struct ResultView {
    ResultType type{ResultType::Complete};
    std::string typeName;
    json::Json result;
};

struct ParsedResult {
    JsonDocument document;
    ResultView result;

    ParsedResult() = default;
    ParsedResult(ParsedResult&&) noexcept = default;
    ParsedResult& operator=(ParsedResult&&) noexcept = default;
    ParsedResult(const ParsedResult&) = delete;
    ParsedResult& operator=(const ParsedResult&) = delete;
};

/** @brief `server/discover` 的可缓存结果。 */
struct DiscoverResult {
    std::vector<std::string> supportedVersions{MCP_VERSION};
    ServerCapabilities capabilities;
    std::optional<std::string> instructions;
    std::optional<Implementation> serverInfo;
    uint64_t ttlMs = 0;
    CacheScope cacheScope{CacheScope::Private};

    std::string to_json() const;
    static std::expected<DiscoverResult, McpError> from_json(const json::Json& element);
};

/** @brief tools/resources/prompts 列表结果的公共编码器。 */
struct ListResult {
    std::string field;
    std::vector<std::string> items;
    std::optional<std::string> nextCursor;
    uint64_t ttlMs = 0;
    CacheScope cacheScope{CacheScope::Private};
    std::optional<Implementation> serverInfo;

    std::string to_json() const;
};

struct JsonRpcRequest {
    RequestId id{int64_t{0}};
    std::string method;
    std::optional<std::string> params;

    std::string to_json() const;
};

/** @brief 工具执行结果。content 中每项必须是一个完整内容块 JSON 对象。 */
struct ToolCallResult {
    std::vector<std::string> content;
    std::optional<std::string> structuredContent;
    bool isError = false;
    std::optional<Implementation> serverInfo;

    static ToolCallResult text(std::string value);
    std::string to_json() const;
};

/** @brief 资源读取结果。contents 中每项必须是 Text/BlobResourceContents JSON。 */
struct ReadResourceResult {
    std::vector<std::string> contents;
    uint64_t ttlMs = 0;
    CacheScope cacheScope{CacheScope::Private};
    std::optional<Implementation> serverInfo;

    static ReadResourceResult text(std::string uri,
                                   std::string value,
                                   std::optional<std::string> mimeType = std::nullopt);
    std::string to_json() const;
};

/** @brief 提示获取结果。messages 中每项必须是完整 PromptMessage JSON。 */
struct GetPromptResult {
    std::vector<std::string> messages;
    std::optional<std::string> description;
    std::optional<Implementation> serverInfo;

    std::string to_json() const;
};

struct RequestView {
    RequestId id{int64_t{0}};
    std::string method;
    json::Json params;
    RequestMeta meta;
};

struct ParsedRequest {
    JsonDocument document;
    RequestView request;

    ParsedRequest() = default;
    ParsedRequest(ParsedRequest&&) noexcept = default;
    ParsedRequest& operator=(ParsedRequest&&) noexcept = default;
    ParsedRequest(const ParsedRequest&) = delete;
    ParsedRequest& operator=(const ParsedRequest&) = delete;
};

struct ResponseView {
    RequestId id{int64_t{0}};
    json::Json result;
    json::Json error;
    bool hasResult = false;
    bool hasError = false;
};

struct ParsedResponse {
    JsonDocument document;
    ResponseView response;

    ParsedResponse() = default;
    ParsedResponse(ParsedResponse&&) noexcept = default;
    ParsedResponse& operator=(ParsedResponse&&) noexcept = default;
    ParsedResponse(const ParsedResponse&) = delete;
    ParsedResponse& operator=(const ParsedResponse&) = delete;
};

/** @brief `subscriptions/listen` 中显式选择的通知集合。 */
struct SubscriptionFilter {
    std::vector<std::string> resourceSubscriptions;
    bool toolsListChanged = false;
    bool resourcesListChanged = false;
    bool promptsListChanged = false;

    std::string to_json() const;
    static std::expected<SubscriptionFilter, McpError> from_json(
        const json::Json& element);
};

/** @brief 构建只包含必需 `_meta` 的请求参数对象。 */
std::string make_request_params(const RequestMeta& meta);

/** @brief 合并业务参数对象与必需 `_meta`；fieldsJson 必须是 JSON 对象。 */
std::expected<std::string, McpError> make_request_params(const RequestMeta& meta,
                                                      std::string_view fieldsJson);

/** @brief 解析并校验 2026-07-28 JSON-RPC 请求和必需的每请求元数据。 */
std::expected<ParsedRequest, McpError> parse_request(std::string_view body);

/** @brief 解析 2026-07-28 结果；该版本缺失 `resultType` 视为错误。 */
std::expected<ParsedResult, McpError> parse_result(std::string_view body);

std::expected<ParsedResponse, McpError> parse_response(std::string_view body);

std::string make_result_response(const RequestId& id, std::string_view resultJson);
std::string make_error_response(const std::optional<RequestId>& id,
                             int code,
                             std::string_view message,
                             std::optional<std::string_view> details = std::nullopt);
std::string make_unsupported_protocol_version_response(
    const RequestId& id,
    std::string_view requested,
    const std::vector<std::string>& supported);

/** @brief 构建 listen 流的首条确认通知。 */
std::string make_subscription_acknowledged_notification(
    const RequestId& id,
    const SubscriptionFilter& accepted);

/** @brief 构建带 subscriptionId 的列表或资源变更通知。 */
std::string make_subscription_notification(
    std::string_view method,
    const RequestId& id,
    std::optional<std::string_view> uri = std::nullopt);

/** @brief 构建服务端主动结束 listen 流时的最终响应。 */
std::string make_subscription_complete_response(const RequestId& id);

/** @brief 把一条 JSON-RPC 消息编码为 SSE data event。 */
std::string encode_sse_event(std::string_view message);

/**
 * @brief 解析一个完整 SSE event。
 * @return data event 返回 JSON 字符串，comment/空 event 返回 nullopt。
 */
std::expected<std::optional<std::string>, McpError> parse_sse_event(
    std::string_view event);

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_PROTOCOL_H
