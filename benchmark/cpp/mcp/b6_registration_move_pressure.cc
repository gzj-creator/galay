/**
 * @file b6_registration_move_pressure.cc
 * @brief MCP 注册路径大 payload 与 handler move 压力基准。
 */

#include <galay/cpp/galay-mcp/server/http_server.h>
#include <galay/cpp/galay-mcp/server/stdio_server.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CopyStats {
    std::atomic<std::size_t> copies{0};
};

struct BenchmarkResult {
    std::size_t callableCopies = 0;
    std::int64_t elapsedUs = 0;
};

bool parseIterations(int argc, char** argv, std::size_t& registrations)
{
    if (argc <= 1) {
        return true;
    }
    std::string_view text(argv[1]);
    std::size_t parsed = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed == 0) {
        std::cerr << "usage: benchmark_mcp_registration_move_pressure [positive-registrations]\n";
        return false;
    }
    registrations = parsed;
    return true;
}

std::string makeLargeSchema()
{
    constexpr std::size_t fieldCount = 32;
    constexpr std::size_t descriptionBytes = 256;
    const std::string description(descriptionBytes, 's');

    std::string schema;
    schema.reserve(fieldCount * (descriptionBytes + 96));
    schema += R"({"type":"object","properties":{)";
    for (std::size_t i = 0; i < fieldCount; ++i) {
        if (i != 0) {
            schema.push_back(',');
        }
        schema += R"("field)";
        schema += std::to_string(i);
        schema += R"(":{"type":"string","description":")";
        schema += description;
        schema += R"("})";
    }
    schema += R"(},"required":["field0","field1"]})";
    return schema;
}

std::vector<galay::mcp::PromptArgument> makeLargeArguments()
{
    constexpr std::size_t argumentCount = 64;
    constexpr std::size_t descriptionBytes = 128;
    std::vector<galay::mcp::PromptArgument> arguments;
    arguments.reserve(argumentCount);
    for (std::size_t i = 0; i < argumentCount; ++i) {
        galay::mcp::PromptArgument argument;
        argument.name = "argument" + std::to_string(i);
        argument.description = std::string(descriptionBytes, 'a');
        argument.required = (i % 3) == 0;
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

class CopyTracked {
public:
    explicit CopyTracked(std::shared_ptr<CopyStats> stats)
        : m_stats(std::move(stats))
    {
    }

    CopyTracked(const CopyTracked& other)
        : m_stats(other.m_stats)
    {
        m_stats->copies.fetch_add(1, std::memory_order_relaxed);
    }

    CopyTracked& operator=(const CopyTracked& other)
    {
        if (this != &other) {
            m_stats = other.m_stats;
            m_stats->copies.fetch_add(1, std::memory_order_relaxed);
        }
        return *this;
    }

    CopyTracked(CopyTracked&&) noexcept = default;
    CopyTracked& operator=(CopyTracked&&) noexcept = default;

protected:
    std::shared_ptr<CopyStats> m_stats;
};

class StdioToolHandler final : public CopyTracked {
public:
    using CopyTracked::CopyTracked;

    std::expected<galay::mcp::JsonString, galay::mcp::McpError>
    operator()(const galay::mcp::JsonElement&) const
    {
        return galay::mcp::JsonString(R"({"ok":true})");
    }
};

class StdioResourceReader final : public CopyTracked {
public:
    using CopyTracked::CopyTracked;

    std::expected<std::string, galay::mcp::McpError> operator()(const std::string&) const
    {
        return std::string("resource");
    }
};

class StdioPromptGetter final : public CopyTracked {
public:
    using CopyTracked::CopyTracked;

    std::expected<galay::mcp::JsonString, galay::mcp::McpError>
    operator()(const std::string&, const galay::mcp::JsonElement&) const
    {
        return galay::mcp::JsonString(R"({"messages":[]})");
    }
};

class HttpToolHandler final : public CopyTracked {
public:
    using CopyTracked::CopyTracked;

    galay::kernel::Task<void>
    operator()(const galay::mcp::JsonElement&,
               std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result) const
    {
        result = galay::mcp::JsonString(R"({"ok":true})");
        co_return;
    }
};

class HttpResourceReader final : public CopyTracked {
public:
    using CopyTracked::CopyTracked;

    galay::kernel::Task<void>
    operator()(const std::string&,
               std::expected<std::string, galay::mcp::McpError>& result) const
    {
        result = std::string("resource");
        co_return;
    }
};

class HttpPromptGetter final : public CopyTracked {
public:
    using CopyTracked::CopyTracked;

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
    return Function{Callable(stats)};
}

BenchmarkResult runStdioRegistration(std::size_t registrations,
                                      const std::string& schemaTemplate,
                                      const std::vector<galay::mcp::PromptArgument>& argumentsTemplate)
{
    auto stats = std::make_shared<CopyStats>();
    galay::mcp::McpStdioServer server;

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < registrations; ++i) {
        const std::string suffix = std::to_string(i);
        auto tool =
            makeCountingFunction<galay::mcp::McpStdioServer::ToolHandler, StdioToolHandler>(stats);
        auto resource =
            makeCountingFunction<galay::mcp::McpStdioServer::ResourceReader, StdioResourceReader>(stats);
        auto prompt =
            makeCountingFunction<galay::mcp::McpStdioServer::PromptGetter, StdioPromptGetter>(stats);

        std::string schema = schemaTemplate;
        std::vector<galay::mcp::PromptArgument> arguments = argumentsTemplate;

        server.addTool("stdio-tool-" + suffix,
                       "Stdio tool " + suffix,
                       std::move(schema),
                       std::move(tool));
        server.addResource("mem://stdio/" + suffix,
                           "stdio-resource-" + suffix,
                           "Stdio resource " + suffix,
                           "text/plain",
                           std::move(resource));
        server.addPrompt("stdio-prompt-" + suffix,
                         "Stdio prompt " + suffix,
                         std::move(arguments),
                         std::move(prompt));
    }
    const auto end = std::chrono::steady_clock::now();

    return BenchmarkResult{
        .callableCopies = stats->copies.load(std::memory_order_relaxed),
        .elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(),
    };
}

BenchmarkResult runHttpRegistration(std::size_t registrations,
                                     const std::string& schemaTemplate,
                                     const std::vector<galay::mcp::PromptArgument>& argumentsTemplate)
{
    auto stats = std::make_shared<CopyStats>();
    galay::mcp::McpHttpServer server("127.0.0.1", 0, 1, 1);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < registrations; ++i) {
        const std::string suffix = std::to_string(i);
        auto tool =
            makeCountingFunction<galay::mcp::McpHttpServer::ToolHandler, HttpToolHandler>(stats);
        auto resource =
            makeCountingFunction<galay::mcp::McpHttpServer::ResourceReader, HttpResourceReader>(stats);
        auto prompt =
            makeCountingFunction<galay::mcp::McpHttpServer::PromptGetter, HttpPromptGetter>(stats);

        std::string schema = schemaTemplate;
        std::vector<galay::mcp::PromptArgument> arguments = argumentsTemplate;

        server.addTool("http-tool-" + suffix,
                       "HTTP tool " + suffix,
                       std::move(schema),
                       std::move(tool));
        server.addResource("mem://http/" + suffix,
                           "http-resource-" + suffix,
                           "HTTP resource " + suffix,
                           "text/plain",
                           std::move(resource));
        server.addPrompt("http-prompt-" + suffix,
                         "HTTP prompt " + suffix,
                         std::move(arguments),
                         std::move(prompt));
    }
    const auto end = std::chrono::steady_clock::now();

    return BenchmarkResult{
        .callableCopies = stats->copies.load(std::memory_order_relaxed),
        .elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(),
    };
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t registrations = 500;
    if (!parseIterations(argc, argv, registrations)) {
        return 2;
    }

    const auto schema = makeLargeSchema();
    const auto arguments = makeLargeArguments();

    const auto stdio = runStdioRegistration(registrations, schema, arguments);
    const auto http = runHttpRegistration(registrations, schema, arguments);
    const auto totalCopies = stdio.callableCopies + http.callableCopies;

    std::cout << "MCP registration move pressure registrations per server: " << registrations << '\n';
    std::cout << "Schema bytes per tool: " << schema.size() << '\n';
    std::cout << "Prompt arguments per prompt: " << arguments.size() << '\n';
    std::cout << "Stdio registration elapsed: " << stdio.elapsedUs << " us\n";
    std::cout << "Stdio callable copies: " << stdio.callableCopies << '\n';
    std::cout << "HTTP registration elapsed: " << http.elapsedUs << " us\n";
    std::cout << "HTTP callable copies: " << http.callableCopies << '\n';
    std::cout << "Total callable copies: " << totalCopies << '\n';

    if (totalCopies != 0) {
        std::cerr << "registration path copied handler callables\n";
        return 1;
    }
    return 0;
}
