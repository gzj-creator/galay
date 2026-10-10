/**
 * @file t13_move_only_ownership_surface.cc
 * @brief 锁定 MCP 拥有状态类型的 move-only 与显式 clone 边界。
 */

#include <galay/cpp/galay-mcp/common/request_codec.h>
#include <galay/cpp/galay-mcp/common/schema_builder.h>

#include <concepts>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

using galay::mcp::ParsedJsonRpcRequest;
using galay::mcp::ParsedJsonRpcResponse;
using galay::mcp::PromptArgumentBuilder;
using galay::mcp::parse_json_rpc_request;
using galay::mcp::parse_json_rpc_response;

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

bool prompt_argument_builder_clone_is_independent()
{
    PromptArgumentBuilder builder;
    builder.add_argument("topic", "Topic", true);

    PromptArgumentBuilder copy = builder.clone();
    builder.add_argument("audience", "Audience", false);

    const auto originalArguments = builder.build();
    const auto cloneArguments = copy.build();
    return require(originalArguments.size() == 2, "original PromptArgumentBuilder did not keep later argument") &&
           require(cloneArguments.size() == 1, "cloned PromptArgumentBuilder observed original's later argument") &&
           require(cloneArguments.front().name == "topic", "cloned PromptArgumentBuilder lost existing argument");
}

bool moved_parsed_request_keeps_views_readable()
{
    auto parsed = parse_json_rpc_request(
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

bool moved_parsed_response_keeps_views_readable()
{
    auto parsed = parse_json_rpc_response(R"({"jsonrpc":"2.0","id":8,"result":{"ok":true}})");
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
    if (!prompt_argument_builder_clone_is_independent()) {
        return 1;
    }
    if (!moved_parsed_request_keeps_views_readable()) {
        return 1;
    }
    if (!moved_parsed_response_keeps_views_readable()) {
        return 1;
    }

    std::cout << "T13-MoveOnlyOwnershipSurface PASS\n";
    return 0;
}
