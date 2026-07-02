/**
 * @file t11_json_document_lifetime_and_allocation.cc
 * @brief 锁定 JsonDocument 返回后的 DOM 生命周期与解析器分配边界。
 */

#include <galay/cpp/galay-mcp/common/mcp_json.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocations{0};

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::size_t takeAllocationCount()
{
    return g_allocations.exchange(0, std::memory_order_relaxed);
}

bool readString(const galay::mcp::JsonDocument& doc, const char* key, std::string_view expected)
{
    auto value = doc.root()[key].get_string();
    if (value.error()) {
        std::cerr << "failed to read key " << key << ": " << simdjson::error_message(value.error()) << '\n';
        return false;
    }
    return require(value.value() == expected, "unexpected string value");
}

bool externalElementSurvivesDocumentMove()
{
    auto parsed = galay::mcp::JsonDocument::parse(R"({"params":{"name":"before-move"}})");
    if (!require(parsed.has_value(), "failed to parse move-alias document")) {
        return false;
    }
    auto params = parsed->root()["params"];
    if (!require(!params.error(), "failed to read params before document move")) {
        return false;
    }
    galay::mcp::JsonElement paramsAlias = params.value();

    galay::mcp::JsonDocument moved = std::move(parsed.value());
    auto name = paramsAlias["name"].get_string();
    if (name.error()) {
        std::cerr << "failed to read aliased element after document move: "
                  << simdjson::error_message(name.error()) << '\n';
        return false;
    }
    return require(name.value() == "before-move", "aliased element changed after document move") &&
           require(!moved.raw().empty(), "moved document lost raw JSON");
}

} // namespace

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed) && size == sizeof(simdjson::dom::parser)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

int main()
{
    const auto first = galay::mcp::JsonDocument::parse(R"({"name":"first","payload":[1,2,3]})");
    if (!require(first.has_value(), "failed to parse first document")) {
        return 1;
    }

    const auto second = galay::mcp::JsonDocument::parse(R"({"name":"second","payload":[4,5,6]})");
    if (!require(second.has_value(), "failed to parse second document")) {
        return 1;
    }

    if (!readString(first.value(), "name", "first")) {
        return 1;
    }
    if (!readString(second.value(), "name", "second")) {
        return 1;
    }
    if (!externalElementSurvivesDocumentMove()) {
        return 1;
    }

    constexpr std::string_view json = R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{"cursor":"abc"}})";
    constexpr std::size_t warmup_iterations = 16;
    constexpr std::size_t measured_iterations = 32;

    for (std::size_t i = 0; i < warmup_iterations; ++i) {
        auto doc = galay::mcp::JsonDocument::parse(json);
        if (!require(doc.has_value(), "warmup parse failed")) {
            return 1;
        }
    }

    takeAllocationCount();
    g_count_allocations.store(true, std::memory_order_relaxed);
    for (std::size_t i = 0; i < measured_iterations; ++i) {
        auto doc = galay::mcp::JsonDocument::parse(json);
        if (!require(doc.has_value(), "measured parse failed")) {
            g_count_allocations.store(false, std::memory_order_relaxed);
            return 1;
        }
    }
    g_count_allocations.store(false, std::memory_order_relaxed);

    const auto parser_allocations = takeAllocationCount();
    if (!require(parser_allocations == 0,
                 "JsonDocument::parse still allocates a simdjson::dom::parser object during steady-state parse")) {
        std::cerr << "parser_allocations=" << parser_allocations
                  << ", iterations=" << measured_iterations << '\n';
        return 1;
    }

    std::cout << "T11-JsonDocumentLifetimeAndAllocation PASS\n";
    return 0;
}
