/**
 * @file t39_hot_path_allocations.cc
 * @brief Lock allocation-free HTTP hot paths.
 */

#include <galay/cpp/galay-http/protoc/http_chunk.h>
#include <galay/cpp/galay-http/protoc/http_header.h>

#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

namespace {

bool g_count_allocations = false;
std::size_t g_allocations = 0;

void* allocate(std::size_t size)
{
    if (g_count_allocations) {
        ++g_allocations;
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    std::abort();
}

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::size_t count_allocations(auto&& operation)
{
    g_allocations = 0;
    g_count_allocations = true;
    operation();
    g_count_allocations = false;
    return g_allocations;
}

bool check_chunk_allocations()
{
    constexpr std::size_t payload_size = 65536;
    const std::string payload(payload_size, 'x');
    const std::string expected_prefix = "10000\r\n";

    std::string from_string;
    const std::size_t from_string_allocations = count_allocations([&] {
        from_string = galay::http::Chunk::to_chunk(payload, false);
    });
    if (!require(from_string_allocations == 1, "Chunk::to_chunk(string) allocation count changed")) {
        return false;
    }
    if (!require(from_string.size() == payload_size + expected_prefix.size() + 2,
                 "Chunk::to_chunk(string) produced an unexpected size")) {
        return false;
    }
    if (!require(std::string_view(from_string).substr(0, expected_prefix.size()) == expected_prefix,
                 "Chunk::to_chunk(string) produced an unexpected prefix")) {
        return false;
    }
    if (!require(std::string_view(from_string).substr(expected_prefix.size(), payload_size) == payload,
                 "Chunk::to_chunk(string) changed the payload")) {
        return false;
    }
    if (!require(std::string_view(from_string).substr(from_string.size() - 2) == "\r\n",
                 "Chunk::to_chunk(string) omitted the final CRLF")) {
        return false;
    }

    std::string from_buffer;
    const std::size_t from_buffer_allocations = count_allocations([&] {
        from_buffer = galay::http::Chunk::to_chunk(payload.data(), payload.size(), false);
    });
    if (!require(from_buffer_allocations == 1, "Chunk::to_chunk(buffer) allocation count changed")) {
        return false;
    }
    return require(from_buffer == from_string, "Chunk::to_chunk overloads disagree");
}

bool check_no_allocation_lookup(const galay::http::HeaderPair& headers,
                                const std::string& key, const std::string* expected)
{
    const std::string* value = nullptr;
    const std::size_t allocations = count_allocations([&] {
        value = headers.get_value_ptr(key);
    });
    if (allocations != 0) {
        std::cerr << "lowercase header lookup allocated: " << key
                  << ", allocations=" << allocations << '\n';
        return false;
    }
    return require(expected == nullptr ? value == nullptr : value != nullptr && *value == *expected,
                   "lowercase header lookup returned an unexpected value");
}

bool check_server_header_allocations()
{
    galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ServerSide);
    const galay::http::HeaderPair empty_headers(galay::http::HeaderPair::Mode::ServerSide);
    // The longest common key exceeds libstdc++'s short-string capacity.
    const std::string common_key = "if-modified-since";
    const std::string common_value = "Wed, 21 Oct 2015 07:28:00 GMT";
    const std::string uncommon_key = "x-uncommon-header-name-for-allocation-test";
    const std::string uncommon_value = "uncommon-value";
    const std::string missing_uncommon_key = "x-missing-header-name-for-allocation-test";
    if (!require(headers.add_header_pair(common_key, common_value) == galay::http::kNoError,
                 "failed to add common header") ||
        !require(headers.add_header_pair(uncommon_key, uncommon_value) == galay::http::kNoError,
                 "failed to add uncommon header")) {
        return false;
    }

    if (!check_no_allocation_lookup(headers, common_key, &common_value) ||
        !check_no_allocation_lookup(empty_headers, common_key, nullptr) ||
        !check_no_allocation_lookup(headers, uncommon_key, &uncommon_value) ||
        !check_no_allocation_lookup(headers, missing_uncommon_key, nullptr)) {
        return false;
    }

    const std::string mixed_common_key = "If-MoDiFiEd-SiNcE";
    const std::string mixed_uncommon_key = "X-Uncommon-Header-Name-For-Allocation-Test";
    const auto* mixed_common_value = headers.get_value_ptr(mixed_common_key);
    const auto* mixed_uncommon_value = headers.get_value_ptr(mixed_uncommon_key);
    return require(mixed_common_value != nullptr && *mixed_common_value == common_value,
                   "mixed-case common header lookup failed") &&
           require(mixed_uncommon_value != nullptr && *mixed_uncommon_value == uncommon_value,
                   "mixed-case uncommon header lookup failed");
}

bool check_client_header_semantics()
{
    galay::http::HeaderPair headers(galay::http::HeaderPair::Mode::ClientSide);
    const std::string key = "x-client-header-name";
    const std::string value = "client-value";
    if (!require(headers.add_header_pair(key, value) == galay::http::kNoError,
                 "failed to add client header")) {
        return false;
    }

    const std::string title_case_key = "X-Client-Header-Name";
    const std::string upper_case_key = "X-CLIENT-HEADER-NAME";
    const std::string missing_key = "X-Missing-Client-Header";
    const auto* lowercase_value = headers.get_value_ptr(key);
    const auto* title_case_value = headers.get_value_ptr(title_case_key);
    const auto* upper_case_value = headers.get_value_ptr(upper_case_key);
    return require(lowercase_value != nullptr && *lowercase_value == value,
                   "client lowercase lookup failed") &&
           require(title_case_value != nullptr && *title_case_value == value,
                   "client title-case lookup failed") &&
           require(upper_case_value != nullptr && *upper_case_value == value,
                   "client uppercase lookup failed") &&
           require(headers.get_value_ptr(missing_key) == nullptr,
                   "client missing header lookup returned a value");
}

} // namespace

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

int main()
{
    if (!check_chunk_allocations() || !check_server_header_allocations() ||
        !check_client_header_semantics()) {
        return 1;
    }
    std::cout << "T39-HotPathAllocations PASS\n";
    return 0;
}
