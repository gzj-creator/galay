/**
 * @file t14_registration_move_ownership.cc
 * @brief 锁定 stdio/http 注册路径按 move 接收 handler，避免注册阶段复制 callable。
 */

#include <galay/cpp/galay-mcp/server/http_server.h>
#include <galay/cpp/galay-mcp/server/stdio_server.h>

#include <atomic>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CopyStats {
    std::atomic<int> copies{0};
};

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

template <typename Result>
class CountingCallable {
public:
    explicit CountingCallable(std::shared_ptr<CopyStats> stats)
        : m_stats(std::move(stats))
    {
    }

    CountingCallable(const CountingCallable& other)
        : m_stats(other.m_stats)
    {
        m_stats->copies.fetch_add(1, std::memory_order_relaxed);
    }

    CountingCallable& operator=(const CountingCallable& other)
    {
        if (this != &other) {
            m_stats = other.m_stats;
            m_stats->copies.fetch_add(1, std::memory_order_relaxed);
        }
        return *this;
    }

    CountingCallable(CountingCallable&&) noexcept = default;
    CountingCallable& operator=(CountingCallable&&) noexcept = default;

protected:
    std::shared_ptr<CopyStats> m_stats;
};

class StdioToolHandler final : public CountingCallable<galay::mcp::JsonString> {
public:
    using CountingCallable::CountingCallable;

    std::expected<galay::mcp::JsonString, galay::mcp::McpError>
    operator()(const galay::mcp::JsonElement&) const
    {
        return galay::mcp::JsonString(R"({"ok":true})");
    }
};

class StdioResourceReader final : public CountingCallable<std::string> {
public:
    using CountingCallable::CountingCallable;

    std::expected<std::string, galay::mcp::McpError> operator()(const std::string&) const
    {
        return std::string("resource");
    }
};

class StdioPromptGetter final : public CountingCallable<galay::mcp::JsonString> {
public:
    using CountingCallable::CountingCallable;

    std::expected<galay::mcp::JsonString, galay::mcp::McpError>
    operator()(const std::string&, const galay::mcp::JsonElement&) const
    {
        return galay::mcp::JsonString(R"({"messages":[]})");
    }
};

class HttpToolHandler final : public CountingCallable<galay::mcp::JsonString> {
public:
    using CountingCallable::CountingCallable;

    galay::kernel::Task<void>
    operator()(const galay::mcp::JsonElement&,
               std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result) const
    {
        result = galay::mcp::JsonString(R"({"ok":true})");
        co_return;
    }
};

class HttpResourceReader final : public CountingCallable<std::string> {
public:
    using CountingCallable::CountingCallable;

    galay::kernel::Task<void>
    operator()(const std::string&,
               std::expected<std::string, galay::mcp::McpError>& result) const
    {
        result = std::string("resource");
        co_return;
    }
};

class HttpPromptGetter final : public CountingCallable<galay::mcp::JsonString> {
public:
    using CountingCallable::CountingCallable;

    galay::kernel::Task<void>
    operator()(const std::string&,
               const galay::mcp::JsonElement&,
               std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result) const
    {
        result = galay::mcp::JsonString(R"({"messages":[]})");
        co_return;
    }
};

template <typename Function, typename Callable>
Function makeCountingFunction(const std::shared_ptr<CopyStats>& stats)
{
    Function fn{Callable(stats)};
    stats->copies.store(0, std::memory_order_relaxed);
    return fn;
}

bool stdioRegistrationMovesHandlers()
{
    auto toolStats = std::make_shared<CopyStats>();
    auto resourceStats = std::make_shared<CopyStats>();
    auto promptStats = std::make_shared<CopyStats>();

    galay::mcp::McpStdioServer server;
    auto tool = makeCountingFunction<galay::mcp::McpStdioServer::ToolHandler, StdioToolHandler>(toolStats);
    auto resource =
        makeCountingFunction<galay::mcp::McpStdioServer::ResourceReader, StdioResourceReader>(resourceStats);
    auto prompt =
        makeCountingFunction<galay::mcp::McpStdioServer::PromptGetter, StdioPromptGetter>(promptStats);

    std::vector<galay::mcp::PromptArgument> arguments;
    arguments.push_back(galay::mcp::PromptArgument{.name = "topic", .description = "Topic", .required = true});

    server.addTool(std::string("echo"), std::string("Echo"), galay::mcp::JsonString("{}"), std::move(tool));
    server.addResource(std::string("mem://one"),
                       std::string("One"),
                       std::string("Resource"),
                       std::string("text/plain"),
                       std::move(resource));
    server.addPrompt(std::string("prompt"), std::string("Prompt"), std::move(arguments), std::move(prompt));

    return require(toolStats->copies.load(std::memory_order_relaxed) == 0,
                   "stdio tool registration copied callable") &&
           require(resourceStats->copies.load(std::memory_order_relaxed) == 0,
                   "stdio resource registration copied callable") &&
           require(promptStats->copies.load(std::memory_order_relaxed) == 0,
                   "stdio prompt registration copied callable");
}

bool httpRegistrationMovesHandlers()
{
    auto toolStats = std::make_shared<CopyStats>();
    auto resourceStats = std::make_shared<CopyStats>();
    auto promptStats = std::make_shared<CopyStats>();

    galay::mcp::McpHttpServer server("127.0.0.1", 0, 1, 1);
    auto tool = makeCountingFunction<galay::mcp::McpHttpServer::ToolHandler, HttpToolHandler>(toolStats);
    auto resource = makeCountingFunction<galay::mcp::McpHttpServer::ResourceReader, HttpResourceReader>(resourceStats);
    auto prompt = makeCountingFunction<galay::mcp::McpHttpServer::PromptGetter, HttpPromptGetter>(promptStats);

    std::vector<galay::mcp::PromptArgument> arguments;
    arguments.push_back(galay::mcp::PromptArgument{.name = "topic", .description = "Topic", .required = true});

    server.addTool(std::string("echo"), std::string("Echo"), galay::mcp::JsonString("{}"), std::move(tool));
    server.addResource(std::string("mem://one"),
                       std::string("One"),
                       std::string("Resource"),
                       std::string("text/plain"),
                       std::move(resource));
    server.addPrompt(std::string("prompt"), std::string("Prompt"), std::move(arguments), std::move(prompt));

    return require(toolStats->copies.load(std::memory_order_relaxed) == 0,
                   "http tool registration copied callable") &&
           require(resourceStats->copies.load(std::memory_order_relaxed) == 0,
                   "http resource registration copied callable") &&
           require(promptStats->copies.load(std::memory_order_relaxed) == 0,
                   "http prompt registration copied callable");
}

} // namespace

int main()
{
    if (!stdioRegistrationMovesHandlers()) {
        return 1;
    }
    if (!httpRegistrationMovesHandlers()) {
        return 1;
    }

    std::cout << "T14-RegistrationMoveOwnership PASS\n";
    return 0;
}
