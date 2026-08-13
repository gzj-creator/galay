/**
 * @file t15_v2_protocol.cc
 * @brief 锁定 MCP 2026-07-28 的无会话元数据、发现与结果边界。
 */

#include <galay/cpp/galay-mcp/v2/common/protocol.h>
#include <galay/cpp/galay-mcp/v2/common/http_headers.h>

#include <iostream>
#include <string>
#include <string_view>
#include <variant>

using namespace galay::mcp;

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    static_assert(std::string_view(v2::MCP_VERSION) == "2026-07-28");
    static_assert(v2::ErrorCodes::HEADER_MISMATCH == -32020);
    static_assert(v2::ErrorCodes::MISSING_REQUIRED_CLIENT_CAPABILITY == -32021);
    static_assert(v2::ErrorCodes::UNSUPPORTED_PROTOCOL_VERSION == -32022);

    v2::RequestMeta meta;
    meta.protocolVersion = v2::MCP_VERSION;
    meta.clientCapabilities = "{}";
    meta.clientInfo = v2::Implementation{.name = "protocol-test", .version = "1.0.0"};

    v2::JsonRpcRequest discover;
    discover.id = 7;
    discover.method = v2::Methods::SERVER_DISCOVER;
    discover.params = v2::makeRequestParams(meta);

    auto parsed = v2::parseRequest(discover.toJson());
    if (!require(parsed.has_value(), "valid v2 discover request was rejected")) {
        return 1;
    }
    if (!require(parsed->request.meta.protocolVersion == v2::MCP_VERSION,
                 "protocol version was not parsed from per-request _meta")) {
        return 1;
    }
    if (!require(parsed->request.meta.clientCapabilities == "{}",
                 "client capabilities were not parsed from per-request _meta")) {
        return 1;
    }

    discover.id = std::string("discover-8");
    auto stringId = v2::parseRequest(discover.toJson());
    if (!require(stringId.has_value() &&
                     std::get<std::string>(stringId->request.id) == "discover-8",
                 "string request id was not preserved")) {
        return 1;
    }

    auto missingMeta = v2::parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    if (!require(!missingMeta.has_value(), "request without required v2 _meta was accepted")) {
        return 1;
    }

    auto malformedCapabilities = v2::parseRequest(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":[]}}})");
    if (!require(!malformedCapabilities.has_value(),
                 "non-object per-request client capabilities were accepted")) {
        return 1;
    }

    auto mergedParams = v2::makeRequestParams(meta, R"({"name":"echo","arguments":{"text":"hi"}})");
    if (!require(mergedParams.has_value() &&
                     mergedParams->find("\"name\":\"echo\"") != std::string::npos &&
                     mergedParams->find("\"_meta\"") != std::string::npos,
                 "business request fields were not merged with v2 _meta")) {
        return 1;
    }
    if (!require(!v2::makeRequestParams(meta, "[]").has_value(),
                 "non-object business request fields were accepted")) {
        return 1;
    }

    auto unsupported = v2::makeUnsupportedProtocolVersionResponse(
        9, "1900-01-01", {v2::MCP_VERSION});
    if (!require(unsupported.find("\"code\":-32022") != std::string::npos &&
                     unsupported.find("\"supported\":[\"2026-07-28\"]") != std::string::npos,
                 "unsupported-version response does not match the v2 schema")) {
        return 1;
    }

    v2::DiscoverResult discovery;
    discovery.supportedVersions = {v2::MCP_VERSION};
    discovery.capabilities.tools = true;
    discovery.capabilities.resources = true;
    discovery.ttlMs = 30000;
    discovery.cacheScope = v2::CacheScope::Public;
    discovery.serverInfo = v2::Implementation{.name = "test-server", .version = "2.0.0"};

    const auto discoveryJson = discovery.toJson();
    if (!require(discoveryJson.find("\"resultType\":\"complete\"") != std::string::npos &&
                     discoveryJson.find("\"ttlMs\":30000") != std::string::npos &&
                     discoveryJson.find("\"cacheScope\":\"public\"") != std::string::npos &&
                     discoveryJson.find("\"supportedVersions\":[\"2026-07-28\"]") != std::string::npos,
                 "discover result is missing required v2 fields")) {
        return 1;
    }

    auto discoveryDocument = JsonDocument::parse(discoveryJson);
    if (!require(discoveryDocument.has_value(), "discover result JSON was malformed")) {
        return 1;
    }
    auto parsedDiscovery = v2::DiscoverResult::fromJson(discoveryDocument->root());
    if (!require(parsedDiscovery.has_value() && parsedDiscovery->capabilities.tools &&
                     parsedDiscovery->capabilities.resources &&
                     parsedDiscovery->cacheScope == v2::CacheScope::Public &&
                     parsedDiscovery->serverInfo.has_value(),
                 "discover result did not round trip")) {
        return 1;
    }

    v2::Tool tool;
    tool.name = "echo";
    tool.title = "Echo";
    tool.description = "Echo text";
    tool.inputSchema = R"({"type":"object","properties":{"text":{"type":"string"}}})";
    tool.outputSchema = R"({"type":"object"})";
    auto toolDocument = JsonDocument::parse(tool.toJson());
    auto parsedTool = toolDocument
        ? v2::Tool::fromJson(toolDocument->root())
        : std::expected<v2::Tool, McpError>(std::unexpected(toolDocument.error()));
    if (!require(parsedTool.has_value() && parsedTool->name == "echo" &&
                     parsedTool->outputSchema.has_value(),
                 "v2 tool metadata did not round trip")) {
        return 1;
    }

    auto invalidToolDocument = JsonDocument::parse(
        R"({"name":"bad","inputSchema":{"type":"string"}})");
    if (!require(invalidToolDocument.has_value() &&
                     !v2::Tool::fromJson(invalidToolDocument->root()).has_value(),
                 "tool inputSchema without object root was accepted")) {
        return 1;
    }

    v2::Resource resource;
    resource.uri = "mem://hello";
    resource.name = "hello";
    resource.mimeType = "text/plain";
    resource.size = 5;
    auto resourceDocument = JsonDocument::parse(resource.toJson());
    auto parsedResource = resourceDocument
        ? v2::Resource::fromJson(resourceDocument->root())
        : std::expected<v2::Resource, McpError>(std::unexpected(resourceDocument.error()));
    if (!require(parsedResource.has_value() && parsedResource->size == 5,
                 "v2 resource metadata did not round trip")) {
        return 1;
    }

    v2::Prompt prompt;
    prompt.name = "review";
    prompt.arguments.push_back(v2::PromptArgument{
        .name = "code", .description = "Code to review", .required = true});
    auto promptDocument = JsonDocument::parse(prompt.toJson());
    auto parsedPrompt = promptDocument
        ? v2::Prompt::fromJson(promptDocument->root())
        : std::expected<v2::Prompt, McpError>(std::unexpected(promptDocument.error()));
    if (!require(parsedPrompt.has_value() && parsedPrompt->arguments.size() == 1 &&
                     parsedPrompt->arguments.front().required,
                 "v2 prompt metadata did not round trip")) {
        return 1;
    }

    v2::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    filter.promptsListChanged = true;
    filter.resourcesListChanged = true;
    filter.resourceSubscriptions = {"mem://hello", "mem://config"};
    auto filterDocument = JsonDocument::parse(filter.toJson());
    auto parsedFilter = filterDocument
        ? v2::SubscriptionFilter::fromJson(filterDocument->root())
        : std::expected<v2::SubscriptionFilter, McpError>(
              std::unexpected(filterDocument.error()));
    if (!require(parsedFilter.has_value() && parsedFilter->toolsListChanged &&
                     parsedFilter->promptsListChanged &&
                     parsedFilter->resourcesListChanged &&
                     parsedFilter->resourceSubscriptions.size() == 2,
                 "subscription filter did not round trip")) {
        return 1;
    }
    auto malformedFilter = JsonDocument::parse(
        R"({"toolsListChanged":"yes","resourceSubscriptions":[7]})");
    if (!require(malformedFilter.has_value() &&
                     !v2::SubscriptionFilter::fromJson(malformedFilter->root()).has_value(),
                 "malformed subscription filter was accepted")) {
        return 1;
    }
    const auto acknowledged = v2::makeSubscriptionAcknowledgedNotification(
        std::string("listen-1"), filter);
    if (!require(acknowledged.find(
                     "\"method\":\"notifications/subscriptions/acknowledged\"") !=
                     std::string::npos &&
                     acknowledged.find(
                         "\"io.modelcontextprotocol/subscriptionId\":\"listen-1\"") !=
                     std::string::npos &&
                     acknowledged.find("\"toolsListChanged\":true") != std::string::npos,
                 "subscription acknowledgement is missing its filter or id")) {
        return 1;
    }
    const auto resourceUpdated = v2::makeSubscriptionNotification(
        v2::NotificationMethods::RESOURCES_UPDATED,
        int64_t{7},
        std::optional<std::string_view>("mem://hello"));
    if (!require(resourceUpdated.find("\"uri\":\"mem://hello\"") !=
                         std::string::npos &&
                     resourceUpdated.find(
                         "\"io.modelcontextprotocol/subscriptionId\":7") !=
                         std::string::npos,
                 "resource update notification is missing its uri or subscription id")) {
        return 1;
    }
    const auto listenComplete = v2::makeSubscriptionCompleteResponse(
        std::string("listen-1"));
    if (!require(listenComplete.find("\"resultType\":\"complete\"") !=
                         std::string::npos &&
                     listenComplete.find(
                         "\"io.modelcontextprotocol/subscriptionId\":\"listen-1\"") !=
                         std::string::npos,
                 "subscription completion response is missing its id")) {
        return 1;
    }
    const auto sseEvent = v2::encodeSseEvent(acknowledged);
    const auto parsedSse = v2::parseSseEvent(sseEvent);
    const auto parsedComment = v2::parseSseEvent(": keep-alive\n\n");
    if (!require(sseEvent.starts_with("data: ") && sseEvent.ends_with("\n\n") &&
                     parsedSse.has_value() && parsedSse->has_value() &&
                     parsedSse->value() == acknowledged && parsedComment.has_value() &&
                     !parsedComment->has_value(),
                 "SSE event codec did not preserve the JSON-RPC message")) {
        return 1;
    }

    v2::ListResult list;
    list.field = "tools";
    list.items = {R"({"name":"echo","inputSchema":{"type":"object"}})"};
    list.ttlMs = 1000;
    list.cacheScope = v2::CacheScope::Private;
    const auto listJson = list.toJson();
    if (!require(listJson.find("\"resultType\":\"complete\"") != std::string::npos &&
                     listJson.find("\"cacheScope\":\"private\"") != std::string::npos,
                 "cacheable list result is missing required v2 fields")) {
        return 1;
    }

    auto complete = v2::parseResult(R"({"resultType":"complete","content":[]})");
    auto inputRequired = v2::parseResult(
        R"({"resultType":"input_required","requestState":"opaque"})");
    auto missingResultType = v2::parseResult(R"({"content":[]})");
    if (!require(complete.has_value() && complete->result.type == v2::ResultType::Complete,
                 "complete result was not parsed")) {
        return 1;
    }
    if (!require(inputRequired.has_value() &&
                     inputRequired->result.type == v2::ResultType::InputRequired,
                 "input-required result was not parsed")) {
        return 1;
    }
    if (!require(!missingResultType.has_value(),
                 "v2 result without required resultType was accepted")) {
        return 1;
    }

    auto callResult = v2::ToolCallResult::text("hello");
    callResult.structuredContent = R"({"echo":"hello"})";
    const auto callResponse = v2::makeResultResponse(std::string("call-1"), callResult.toJson());
    auto parsedCallResponse = v2::parseResponse(callResponse);
    if (!require(parsedCallResponse.has_value() && parsedCallResponse->response.hasResult &&
                     !parsedCallResponse->response.hasError &&
                     std::get<std::string>(parsedCallResponse->response.id) == "call-1",
                 "successful v2 response was not parsed")) {
        return 1;
    }

    auto parsedErrorResponse = v2::parseResponse(
        v2::makeErrorResponse(int64_t{3}, v2::ErrorCodes::INVALID_PARAMS, "bad params"));
    if (!require(parsedErrorResponse.has_value() &&
                     !parsedErrorResponse->response.hasResult &&
                     parsedErrorResponse->response.hasError,
                 "v2 error response was not parsed")) {
        return 1;
    }
    if (!require(!v2::parseResponse(
                      R"({"jsonrpc":"2.0","id":1,"result":{"content":[]}})")
                      .has_value(),
                 "successful v2 response without resultType was accepted")) {
        return 1;
    }

    v2::Tool headerTool;
    headerTool.name = "header-tool";
    headerTool.inputSchema = R"({"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"},"count":{"type":"integer","x-mcp-header":"Count"},"enabled":{"type":"boolean","x-mcp-header":"Enabled"}}})";
    auto annotations = v2::toolHeaderAnnotations(headerTool);
    if (!require(annotations.has_value() && annotations->size() == 3,
                 "valid x-mcp-header annotations were rejected")) {
        return 1;
    }
    auto argumentsDocument = JsonDocument::parse(
        R"({"region":"us west","count":42,"enabled":true})");
    if (!require(argumentsDocument.has_value(), "header argument JSON was malformed")) {
        return 1;
    }
    for (const auto& annotation : annotations.value()) {
        auto value = v2::argumentHeaderValue(argumentsDocument->root(), annotation);
        if (!require(value.has_value() && value->has_value(),
                     "annotated argument was not extracted")) {
            return 1;
        }
        if (annotation.name == "Region" && !require(value.value() == "us west",
                                                      "string header argument mismatch")) {
            return 1;
        }
        if (annotation.name == "Count" && !require(value.value() == "42",
                                                     "integer header argument mismatch")) {
            return 1;
        }
        if (annotation.name == "Enabled" && !require(value.value() == "true",
                                                       "boolean header argument mismatch")) {
            return 1;
        }
    }
    const std::string encoded = v2::encodeHeaderValue(" leading value ");
    auto decoded = v2::decodeHeaderValue(encoded);
    if (!require(decoded.has_value() && decoded.value() == " leading value ",
                 "mirrored header value encoding did not round trip")) {
        return 1;
    }
    v2::Tool invalidHeaderTool;
    invalidHeaderTool.name = "invalid-header-tool";
    invalidHeaderTool.inputSchema = R"({"type":"object","properties":{"items":{"type":"array","items":{"type":"string","x-mcp-header":"Bad"}}}})";
    if (!require(!v2::toolHeaderAnnotations(invalidHeaderTool).has_value(),
                 "array-reachable x-mcp-header annotation was accepted")) {
        return 1;
    }

    const auto readResult = v2::ReadResourceResult::text("mem://hello", "hello", "text/plain");
    const auto readJson = readResult.toJson();
    if (!require(readJson.find("\"resultType\":\"complete\"") != std::string::npos &&
                     readJson.find("\"uri\":\"mem://hello\"") != std::string::npos &&
                     readJson.find("\"text\":\"hello\"") != std::string::npos,
                 "v2 resource read result is missing required fields")) {
        return 1;
    }

    v2::GetPromptResult promptResult;
    promptResult.messages = {R"({"role":"user","content":{"type":"text","text":"Review"}})"};
    if (!require(promptResult.toJson().find("\"resultType\":\"complete\"") !=
                     std::string::npos,
                 "v2 prompt result is missing resultType")) {
        return 1;
    }

    std::cout << "T15-V2Protocol PASS\n";
    return 0;
}
