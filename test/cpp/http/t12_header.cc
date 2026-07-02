/**
 * @file T39-HeaderFastPath.cc
 * @brief HeaderPair fast-path 功能单元测试
 *
 * 测试场景：
 * 1. 常见 header 的设置和获取
 * 2. 重复 header 的合并（RFC 7230）
 * 3. 常见 header 的存在性检查
 * 4. 遍历所有 header（包括 common headers 和 map）
 */

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <galay/cpp/galay-http/protoc/http_header.h>

using namespace galay::http;

// 测试统计
static int g_passed = 0;
static int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAILED] " << msg << " at line " << __LINE__ << std::endl; \
            g_failed++; \
            return; \
        } \
    } while(0)

#define TEST_PASS(name) \
    do { \
        std::cout << "[PASSED] " << name << std::endl; \
        g_passed++; \
    } while(0)

// ============ Fast-path 基础功能测试 ============

void test_set_and_get_common_header()
{
    std::cout << "\n=== Test: Set and get common header ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 测试设置常见 header
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "123");
    headers.setCommonHeader(CommonHeaderIndex::ContentType, "application/json");

    // 测试获取
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Host) == "example.com",
                "Host should match");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::ContentLength) == "123",
                "Content-Length should match");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::ContentType) == "application/json",
                "Content-Type should match");

    // 测试不存在的
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Cookie) == "",
                "Non-existent header should return empty string");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Authorization) == "",
                "Non-existent header should return empty string");

    TEST_PASS("Set and get common header");
}

void test_duplicate_header_merge()
{
    std::cout << "\n=== Test: Duplicate header merge ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 设置第一个 Accept header
    headers.setCommonHeader(CommonHeaderIndex::Accept, "text/html");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Accept) == "text/html",
                "First Accept should be set");

    // 设置第二个 Accept header（应该合并）
    headers.setCommonHeader(CommonHeaderIndex::Accept, "application/json");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Accept) == "text/html, application/json",
                "Second Accept should be merged with comma");

    // 设置第三个 Accept header
    headers.setCommonHeader(CommonHeaderIndex::Accept, "text/plain");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Accept) == "text/html, application/json, text/plain",
                "Third Accept should be merged");

    TEST_PASS("Duplicate header merge");
}

void test_has_common_header()
{
    std::cout << "\n=== Test: Has common header ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 初始状态：没有任何 header
    TEST_ASSERT(!headers.hasCommonHeader(CommonHeaderIndex::Host),
                "Host should not exist initially");
    TEST_ASSERT(!headers.hasCommonHeader(CommonHeaderIndex::ContentLength),
                "Content-Length should not exist initially");

    // 设置 Host header
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Host),
                "Host should exist after setting");
    TEST_ASSERT(!headers.hasCommonHeader(CommonHeaderIndex::Cookie),
                "Cookie should still not exist");

    // 设置更多 headers
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "100");
    headers.setCommonHeader(CommonHeaderIndex::UserAgent, "TestAgent/1.0");

    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Host),
                "Host should still exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::ContentLength),
                "Content-Length should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::UserAgent),
                "User-Agent should exist");
    TEST_ASSERT(!headers.hasCommonHeader(CommonHeaderIndex::Authorization),
                "Authorization should not exist");

    TEST_PASS("Has common header");
}

void test_server_side_common_header_lookup_api()
{
    std::cout << "\n=== Test: Server-side common header lookup API ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);
    headers.setCommonHeader(CommonHeaderIndex::Connection, "Upgrade");
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");

    TEST_ASSERT(headers.hasKey("Connection"),
                "Server-side hasKey should see fast-path Connection header");
    TEST_ASSERT(headers.hasKey("connection"),
                "Server-side hasKey should accept normalized Connection header");
    TEST_ASSERT(headers.hasKey("Host"),
                "Server-side hasKey should see fast-path Host header");
    TEST_ASSERT(headers.getValue("Connection") == "Upgrade",
                "Server-side getValue should return fast-path Connection header");
    TEST_ASSERT(headers.getValue("host") == "example.com",
                "Server-side getValue should return fast-path Host header");

    TEST_PASS("Server-side common header lookup API");
}

void test_server_side_remove_common_header()
{
    std::cout << "\n=== Test: Server-side remove common header ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::Connection, "close");

    TEST_ASSERT(headers.removeHeaderPair("Host") == kNoError,
                "Removing fast-path Host header should succeed");
    TEST_ASSERT(headers.removeHeaderPair("connection") == kNoError,
                "Removing fast-path Connection header should succeed");
    TEST_ASSERT(!headers.hasKey("Host"),
                "Removed fast-path Host header should not remain visible");
    TEST_ASSERT(!headers.hasKey("connection"),
                "Removed fast-path Connection header should not remain visible");
    TEST_ASSERT(headers.getValue("Host").empty(),
                "Removed fast-path Host header should not return a value");
    TEST_ASSERT(headers.getValue("connection").empty(),
                "Removed fast-path Connection header should not return a value");

    std::string serialized = headers.toString();
    TEST_ASSERT(serialized.find("host:") == std::string::npos,
                "Serialized headers should not contain removed Host");
    TEST_ASSERT(serialized.find("connection:") == std::string::npos,
                "Serialized headers should not contain removed Connection");

    TEST_PASS("Server-side remove common header");
}

void test_server_side_add_if_not_exist_respects_common_header()
{
    std::cout << "\n=== Test: Server-side add-if-not-exist respects common header ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "9");

    TEST_ASSERT(headers.addHeaderPairIfNotExist("Content-Length", "9") == kHeaderPairExist,
                "Fast-path Content-Length should be treated as existing");

    std::string serialized = headers.toString();
    size_t first = serialized.find("content-length:");
    TEST_ASSERT(first != std::string::npos,
                "Serialized headers should contain Content-Length");
    TEST_ASSERT(serialized.find("content-length:", first + 1) == std::string::npos,
                "Serialized headers should not duplicate Content-Length");

    TEST_PASS("Server-side add-if-not-exist respects common header");
}

void test_for_each_header()
{
    std::cout << "\n=== Test: For each header ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 设置一些 common headers
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "100");
    headers.setCommonHeader(CommonHeaderIndex::UserAgent, "TestAgent/1.0");

    // 设置一些非 common headers
    headers.addHeaderPair("x-custom-header", "custom-value");
    headers.addHeaderPair("x-another-header", "another-value");

    // 收集所有 headers
    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    // 验证总数
    TEST_ASSERT(result.size() == 5, "Should have 5 headers total");

    // 验证包含所有 header
    bool has_host = false, has_length = false, has_ua = false;
    bool has_custom = false, has_another = false;

    for (const auto& [k, v] : result) {
        if (k == "host" && v == "example.com") has_host = true;
        if (k == "content-length" && v == "100") has_length = true;
        if (k == "user-agent" && v == "TestAgent/1.0") has_ua = true;
        if (k == "x-custom-header" && v == "custom-value") has_custom = true;
        if (k == "x-another-header" && v == "another-value") has_another = true;
    }

    TEST_ASSERT(has_host, "Should contain host header");
    TEST_ASSERT(has_length, "Should contain content-length header");
    TEST_ASSERT(has_ua, "Should contain user-agent header");
    TEST_ASSERT(has_custom, "Should contain x-custom-header");
    TEST_ASSERT(has_another, "Should contain x-another-header");

    TEST_PASS("For each header");
}

void test_for_each_header_empty()
{
    std::cout << "\n=== Test: For each header (empty) ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    int count = 0;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        count++;
    });

    TEST_ASSERT(count == 0, "Empty headers should not invoke callback");

    TEST_PASS("For each header (empty)");
}

void test_for_each_header_only_common()
{
    std::cout << "\n=== Test: For each header (only common) ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentType, "text/html");

    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    TEST_ASSERT(result.size() == 2, "Should have 2 common headers");

    TEST_PASS("For each header (only common)");
}

void test_for_each_header_only_map()
{
    std::cout << "\n=== Test: For each header (only map) ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    headers.addHeaderPair("x-custom-1", "value1");
    headers.addHeaderPair("x-custom-2", "value2");
    headers.addHeaderPair("x-custom-3", "value3");

    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    TEST_ASSERT(result.size() == 3, "Should have 3 map headers");

    TEST_PASS("For each header (only map)");
}

// ============ 边界情况测试 ============

void test_empty_value()
{
    std::cout << "\n=== Test: Empty value ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    headers.setCommonHeader(CommonHeaderIndex::Host, "");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Host),
                "Header with empty value should exist");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Host) == "",
                "Empty value should be retrievable");

    TEST_PASS("Empty value");
}

void test_overwrite_value()
{
    std::cout << "\n=== Test: Overwrite value ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    headers.setCommonHeader(CommonHeaderIndex::Host, "first.com");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Host) == "first.com",
                "First value should be set");

    headers.setCommonHeader(CommonHeaderIndex::Host, "second.com");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Host) == "first.com, second.com",
                "Second value should be merged");

    TEST_PASS("Overwrite value");
}

void test_all_common_headers()
{
    std::cout << "\n=== Test: All common headers ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 设置所有 common headers
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "123");
    headers.setCommonHeader(CommonHeaderIndex::ContentType, "text/html");
    headers.setCommonHeader(CommonHeaderIndex::UserAgent, "TestAgent");
    headers.setCommonHeader(CommonHeaderIndex::Accept, "text/html");
    headers.setCommonHeader(CommonHeaderIndex::AcceptEncoding, "gzip");
    headers.setCommonHeader(CommonHeaderIndex::Connection, "keep-alive");
    headers.setCommonHeader(CommonHeaderIndex::CacheControl, "no-cache");
    headers.setCommonHeader(CommonHeaderIndex::Cookie, "session=abc");
    headers.setCommonHeader(CommonHeaderIndex::Authorization, "Bearer token");
    headers.setCommonHeader(CommonHeaderIndex::IfModifiedSince, "Wed, 21 Oct 2015 07:28:00 GMT");
    headers.setCommonHeader(CommonHeaderIndex::IfNoneMatch, "\"abc123\"");
    headers.setCommonHeader(CommonHeaderIndex::Referer, "https://example.com");
    headers.setCommonHeader(CommonHeaderIndex::AcceptLanguage, "en-US");
    headers.setCommonHeader(CommonHeaderIndex::Range, "bytes=0-1023");

    // 验证所有都存在
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Host), "Host should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::ContentLength), "ContentLength should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::ContentType), "ContentType should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::UserAgent), "UserAgent should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Accept), "Accept should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::AcceptEncoding), "AcceptEncoding should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Connection), "Connection should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::CacheControl), "CacheControl should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Cookie), "Cookie should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Authorization), "Authorization should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::IfModifiedSince), "IfModifiedSince should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::IfNoneMatch), "IfNoneMatch should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Referer), "Referer should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::AcceptLanguage), "AcceptLanguage should exist");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Range), "Range should exist");

    // 验证 forEachHeader 包含所有
    int count = 0;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        count++;
    });
    TEST_ASSERT(count == 15, "Should have 15 common headers");

    TEST_PASS("All common headers");
}

void test_mixed_common_and_map()
{
    std::cout << "\n=== Test: Mixed common and map headers ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    // 混合设置 common 和 map headers
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.addHeaderPair("x-custom-1", "value1");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "100");
    headers.addHeaderPair("x-custom-2", "value2");
    headers.setCommonHeader(CommonHeaderIndex::UserAgent, "TestAgent");

    // 验证 common headers
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Host) == "example.com",
                "Host should be accessible");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::ContentLength) == "100",
                "Content-Length should be accessible");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::UserAgent) == "TestAgent",
                "User-Agent should be accessible");

    // 验证 map headers
    TEST_ASSERT(headers.getValue("x-custom-1") == "value1",
                "x-custom-1 should be accessible");
    TEST_ASSERT(headers.getValue("x-custom-2") == "value2",
                "x-custom-2 should be accessible");

    // 验证总数
    int count = 0;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        count++;
    });
    TEST_ASSERT(count == 5, "Should have 5 headers total");

    TEST_PASS("Mixed common and map headers");
}

void test_request_parse_merges_duplicate_rare_headers()
{
    std::cout << "\n=== Test: Request parse merges duplicate rare headers ===" << std::endl;

    HttpRequestHeader header;
    const std::string raw =
        "GET /rare HTTP/1.1\r\n"
        "X-Trace-Id: first\r\n"
        "x-trace-id: second\r\n"
        "\r\n";

    auto [err, consumed] = header.fromString(raw);
    TEST_ASSERT(err == kNoError, "Rare duplicate header parse should succeed");
    TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()),
                "Rare duplicate header parse should consume the full header");
    TEST_ASSERT(header.headerPairs().getValue("x-trace-id") == "first, second",
                "Rare duplicate request headers should be merged in parse order");
    TEST_ASSERT(header.headerPairs().getValue("X-Trace-Id") == "first, second",
                "Rare duplicate request headers should remain case-insensitive");

    TEST_PASS("Request parse merges duplicate rare headers");
}

void test_client_side_mode()
{
    std::cout << "\n=== Test: Client side mode ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ClientSide);

    // ClientSide 模式下，fast-path 不应该被使用
    // 但 API 仍然应该工作（通过 fallback 到 map）
    headers.setCommonHeader(CommonHeaderIndex::Host, "example.com");
    headers.setCommonHeader(CommonHeaderIndex::ContentLength, "100");

    // 这些调用应该正常工作（即使内部可能不使用 fast-path）
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Host),
                "Host should exist in ClientSide mode");
    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Host) == "example.com",
                "Host should be retrievable in ClientSide mode");

    TEST_PASS("Client side mode");
}

void test_large_value()
{
    std::cout << "\n=== Test: Large value ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    std::string large_value(10000, 'x');
    headers.setCommonHeader(CommonHeaderIndex::Cookie, large_value);

    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::Cookie) == large_value,
                "Large value should be stored and retrieved correctly");
    TEST_ASSERT(headers.hasCommonHeader(CommonHeaderIndex::Cookie),
                "Header with large value should exist");

    TEST_PASS("Large value");
}

void test_special_characters()
{
    std::cout << "\n=== Test: Special characters ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    std::string special = "value with spaces, commas; semicolons: colons";
    headers.setCommonHeader(CommonHeaderIndex::UserAgent, special);

    TEST_ASSERT(headers.getCommonHeader(CommonHeaderIndex::UserAgent) == special,
                "Special characters should be preserved");

    TEST_PASS("Special characters");
}

// ============ 主函数 ============

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "HeaderPair Fast-Path Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // 基础功能测试
    test_set_and_get_common_header();
    test_duplicate_header_merge();
    test_has_common_header();
    test_server_side_common_header_lookup_api();
    test_server_side_remove_common_header();
    test_server_side_add_if_not_exist_respects_common_header();
    test_for_each_header();
    test_for_each_header_empty();
    test_for_each_header_only_common();
    test_for_each_header_only_map();

    // 边界情况测试
    test_empty_value();
    test_overwrite_value();
    test_all_common_headers();
    test_mixed_common_and_map();
    test_request_parse_merges_duplicate_rare_headers();
    test_client_side_mode();
    test_large_value();
    test_special_characters();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
