/**
 * @file t13_move_only_ownership_surface.cc
 * @brief 锁定 MCP 拥有状态类型的 move-only 与显式 clone 边界。
 */

#include <galay/cpp/galay-mcp/common/json_parser.h>
#include <galay/cpp/galay-mcp/common/schema_builder.h>

#include <concepts>
#include <iostream>
#include <string_view>
#include <type_traits>

using galay::mcp::JsonWriter;
using galay::mcp::ParsedJsonRpcRequest;
using galay::mcp::ParsedJsonRpcResponse;
using galay::mcp::PromptArgumentBuilder;
using galay::mcp::SchemaBuilder;
using galay::mcp::parseJsonRpcRequest;
using galay::mcp::parseJsonRpcResponse;

static_assert(!std::copy_constructible<JsonWriter>);
static_assert(!std::is_copy_assignable_v<JsonWriter>);
static_assert(std::movable<JsonWriter>);
static_assert(std::is_nothrow_move_constructible_v<JsonWriter>);
static_assert(std::is_nothrow_move_assignable_v<JsonWriter>);

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

static_assert(requires(const JsonWriter& writer) {
    { writer.clone() } -> std::same_as<JsonWriter>;
});
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

bool jsonWriterCloneIsIndependent()
{
    JsonWriter writer;
    writer.startObject();
    writer.key("before");
    writer.string("copy");

    JsonWriter copy = writer.clone();

    writer.key("after");
    writer.string("original");
    writer.endObject();

    copy.key("after");
    copy.string("clone");
    copy.endObject();

    const auto originalJson = writer.takeString();
    const auto cloneJson = copy.takeString();
    return require(contains(originalJson, R"("after":"original")"),
                   "original JsonWriter did not keep later mutation") &&
           require(contains(cloneJson, R"("after":"clone")"),
                   "cloned JsonWriter did not accept independent mutation") &&
           require(!contains(cloneJson, "original"),
                   "cloned JsonWriter observed original's later mutation");
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

    auto name = moved.request.params["name"].get_string();
    if (name.error()) {
        std::cerr << "failed to read moved request params name: "
                  << simdjson::error_message(name.error()) << '\n';
        return false;
    }
    return require(name.value() == "echo", "moved request params view changed");
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

    auto ok = moved.response.result["ok"].get_bool();
    if (ok.error()) {
        std::cerr << "failed to read moved response result ok: "
                  << simdjson::error_message(ok.error()) << '\n';
        return false;
    }
    return require(ok.value(), "moved response result view changed");
}

} // namespace

int main()
{
    if (!jsonWriterCloneIsIndependent()) {
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
