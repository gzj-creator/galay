/**
 * @file t13_move_only_ownership_surface.cc
 * @brief 锁定 MCP 拥有状态类型的 move-only 与显式 clone 边界。
 */

#include <galay/cpp/galay-mcp/common/json_parser.h>
#include <galay/cpp/galay-mcp/common/schema_builder.h>

#include <concepts>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

using galay::mcp::ParsedJsonRpcRequest;
using galay::mcp::ParsedJsonRpcResponse;
using galay::mcp::PromptArgumentBuilder;
using galay::mcp::SchemaBuilder;
using galay::mcp::parseJsonRpcRequest;
using galay::mcp::parseJsonRpcResponse;

static_assert(!std::copy_constructible<json::stream::StreamWriter>);
static_assert(!std::is_copy_assignable_v<json::stream::StreamWriter>);
static_assert(std::movable<json::stream::StreamWriter>);
static_assert(std::is_nothrow_move_constructible_v<json::stream::StreamWriter>);
static_assert(std::is_nothrow_move_assignable_v<json::stream::StreamWriter>);

static_assert(!std::copy_constructible<SchemaBuilder>);
static_assert(!std::is_copy_assignable_v<SchemaBuilder>);
static_assert(std::movable<SchemaBuilder>);
static_assert(std::is_nothrow_move_constructible_v<SchemaBuilder>);
static_assert(std::is_nothrow_move_assignable_v<SchemaBuilder>);

static_assert(!std::copy_constructible<PromptArgumentBuilder>);
static_assert(!std::is_copy_assignable_v<PromptArgumentBuilder>);
static_assert(std::movable<PromptArgumentBuilder>);
static_assert(std::is_nothrow_move_constructible_v<PromptArgumentBuilder>);
static_assert(std::is_nothrow_move_assignable_v<PromptArgumentBuilder>);

static_assert(!std::copy_constructible<ParsedJsonRpcRequest>);
static_assert(!std::is_copy_assignable_v<ParsedJsonRpcRequest>);
static_assert(std::movable<ParsedJsonRpcRequest>);

static_assert(!std::copy_constructible<ParsedJsonRpcResponse>);
static_assert(!std::is_copy_assignable_v<ParsedJsonRpcResponse>);
static_assert(std::movable<ParsedJsonRpcResponse>);

static_assert(requires(const SchemaBuilder& builder) {
    { builder.clone() } -> std::same_as<SchemaBuilder>;
});
static_assert(requires(const PromptArgumentBuilder& builder) {
    { builder.clone() } -> std::same_as<PromptArgumentBuilder>;
});

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

bool jsonWriterSinksAreIndependent()
{
    std::string originalJson;
    std::string cloneJson;
    auto writer = galay::mcp::makeJsonWriter(originalJson);
    auto copy = galay::mcp::makeJsonWriter(cloneJson);

    (void)writer.start_object();
    (void)writer.key("before");
    (void)writer.string("copy");
    (void)writer.key("after");
    (void)writer.string("original");
    (void)writer.end_object();

    (void)copy.start_object();
    (void)copy.key("before");
    (void)copy.string("copy");
    (void)copy.key("after");
    (void)copy.string("clone");
    (void)copy.end_object();

    if (!require(writer.finish().has_value(), "first JSON writer failed to finish") ||
        !require(copy.finish().has_value(), "second JSON writer failed to finish")) {
        return false;
    }

    return require(contains(originalJson, R"("after":"original")"),
                   "first JSON writer did not keep later mutation") &&
           require(contains(cloneJson, R"("after":"clone")"),
                   "second JSON writer did not accept independent mutation") &&
           require(!contains(cloneJson, "original"),
                   "second JSON writer observed the first writer's later mutation");
}

bool schemaBuilderCloneIsIndependent()
{
    SchemaBuilder builder;
    builder.addString("name", "Name", true);

    SchemaBuilder copy = builder.clone();
    builder.addInteger("age", "Age", false);

    const auto originalSchema = builder.build();
    const auto cloneSchema = copy.build();
    return require(contains(originalSchema, R"("age")"),
                   "original SchemaBuilder did not keep later property") &&
           require(!contains(cloneSchema, R"("age")"),
                   "cloned SchemaBuilder observed original's later property") &&
           require(contains(cloneSchema, R"("name")"),
                   "cloned SchemaBuilder lost existing property");
}

bool promptArgumentBuilderCloneIsIndependent()
{
    PromptArgumentBuilder builder;
    builder.addArgument("topic", "Topic", true);

    PromptArgumentBuilder copy = builder.clone();
    builder.addArgument("audience", "Audience", false);

    const auto originalArguments = builder.build();
    const auto cloneArguments = copy.build();
    return require(originalArguments.size() == 2, "original PromptArgumentBuilder did not keep later argument") &&
           require(cloneArguments.size() == 1, "cloned PromptArgumentBuilder observed original's later argument") &&
           require(cloneArguments.front().name == "topic", "cloned PromptArgumentBuilder lost existing argument");
}

bool movedParsedRequestKeepsViewsReadable()
{
    auto parsed = parseJsonRpcRequest(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hello"}}})");
    if (!require(parsed.has_value(), "failed to parse JSON-RPC request")) {
        return false;
    }

    ParsedJsonRpcRequest moved = std::move(parsed.value());
    if (!require(moved.request.method == "tools/call", "moved request lost method")) {
        return false;
    }

    auto name = moved.request.params.at("name").as_string();
    if (!name) {
        std::cerr << "failed to read moved request params name: "
                  << name.error() << '\n';
        return false;
    }
    return require(*name == "echo", "moved request params view changed");
}

bool movedParsedResponseKeepsViewsReadable()
{
    auto parsed = parseJsonRpcResponse(R"({"jsonrpc":"2.0","id":8,"result":{"ok":true}})");
    if (!require(parsed.has_value(), "failed to parse JSON-RPC response")) {
        return false;
    }

    ParsedJsonRpcResponse moved = std::move(parsed.value());
    if (!require(moved.response.id == 8 && moved.response.hasResult, "moved response lost metadata")) {
        return false;
    }

    auto ok = moved.response.result.at("ok").as_bool();
    if (!ok) {
        std::cerr << "failed to read moved response result ok: "
                  << ok.error() << '\n';
        return false;
    }
    return require(*ok, "moved response result view changed");
}

} // namespace

int main()
{
    if (!jsonWriterSinksAreIndependent()) {
        return 1;
    }
    if (!schemaBuilderCloneIsIndependent()) {
        return 1;
    }
    if (!promptArgumentBuilderCloneIsIndependent()) {
        return 1;
    }
    if (!movedParsedRequestKeepsViewsReadable()) {
        return 1;
    }
    if (!movedParsedResponseKeepsViewsReadable()) {
        return 1;
    }

    std::cout << "T13-MoveOnlyOwnershipSurface PASS\n";
    return 0;
}
