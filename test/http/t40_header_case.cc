/**
 * @file T40-ClientHeaderCase.cc
 * @brief Client 端 Header 规范化测试
 *
 * 测试场景：
 * 1. Client 端将 headers 规范化为 Title-Case
 * 2. Client 端不使用 fast-path
 * 3. Client 端正确处理各种输入大小写
 */

#include <iostream>
#include <cassert>
#include <vector>
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

void test_client_normalizes_to_canonical_case()
{
    std::cout << "\n=== Test: Client normalizes to canonical case ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ClientSide);

    headers.addHeaderPair("Content-Type", "application/json");
    headers.addHeaderPair("X-Custom-Header", "value");
    headers.addHeaderPair("Authorization", "Bearer token");

    // 验证规范化为 Title-Case
    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    TEST_ASSERT(result.size() == 3, "Should have 3 headers");

    // 查找并验证规范化后的大小写
    bool found_content_type = false;
    bool found_custom = false;
    bool found_auth = false;

    for (const auto& [k, v] : result) {
        if (k == "Content-Type") {
            found_content_type = true;
            TEST_ASSERT(v == "application/json", "Content-Type value should match");
        }
        if (k == "X-Custom-Header") {
            found_custom = true;
            TEST_ASSERT(v == "value", "X-Custom-Header value should match");
        }
        if (k == "Authorization") {
            found_auth = true;
            TEST_ASSERT(v == "Bearer token", "Authorization value should match");
        }
    }

    TEST_ASSERT(found_content_type, "Should find Content-Type in canonical case");
    TEST_ASSERT(found_custom, "Should find X-Custom-Header in canonical case");
    TEST_ASSERT(found_auth, "Should find Authorization in canonical case");

    TEST_PASS("Client normalizes to canonical case");
}

void test_client_no_fast_path_used()
{
    std::cout << "\n=== Test: Client does not use fast-path ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ClientSide);

    // Client 端不应该使用 fast-path
    headers.addHeaderPair("Host", "example.com");
    headers.addHeaderPair("Content-Length", "100");

    // 验证存储在 map 中，不在 common headers 中
    TEST_ASSERT(!headers.hasCommonHeader(CommonHeaderIndex::Host),
                "Host should not be in common headers for ClientSide");
    TEST_ASSERT(!headers.hasCommonHeader(CommonHeaderIndex::ContentLength),
                "Content-Length should not be in common headers for ClientSide");

    // 但可以通过 getValue 查询到
    TEST_ASSERT(headers.getValue("Host") == "example.com",
                "Should find Host via getValue");
    TEST_ASSERT(headers.getValue("Content-Length") == "100",
                "Should find Content-Length via getValue");

    TEST_PASS("Client does not use fast-path");
}

void test_client_normalizes_mixed_case_input()
{
    std::cout << "\n=== Test: Client normalizes mixed case input ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ClientSide);

    // 添加各种大小写的 headers，应该都被规范化为 Title-Case
    headers.addHeaderPair("content-type", "text/html");
    headers.addHeaderPair("CONTENT-LENGTH", "200");
    headers.addHeaderPair("Accept-Encoding", "gzip, deflate");
    headers.addHeaderPair("x-api-key", "secret123");

    // 验证所有 headers 都被规范化为 Title-Case
    std::vector<std::pair<std::string, std::string>> result;
    headers.forEachHeader([&](std::string_view k, std::string_view v) {
        result.emplace_back(k, v);
    });

    TEST_ASSERT(result.size() == 4, "Should have 4 headers");

    // Debug: print all headers
    std::cout << "Headers found:" << std::endl;
    for (const auto& [k, v] : result) {
        std::cout << "  '" << k << "' = '" << v << "'" << std::endl;
    }

    bool found_content_type = false;
    bool found_content_length = false;
    bool found_accept_encoding = false;
    bool found_api_key = false;

    for (const auto& [k, v] : result) {
        // 所有 headers 应该被规范化为 Title-Case
        if (k == "Content-Type") {
            found_content_type = true;
            TEST_ASSERT(v == "text/html", "Content-Type value should match");
        }
        if (k == "Content-Length") {
            found_content_length = true;
            TEST_ASSERT(v == "200", "Content-Length value should match");
        }
        if (k == "Accept-Encoding") {
            found_accept_encoding = true;
            TEST_ASSERT(v == "gzip, deflate", "Accept-Encoding value should match");
        }
        if (k == "X-Api-Key") {
            found_api_key = true;
            TEST_ASSERT(v == "secret123", "X-Api-Key value should match");
        }
    }

    TEST_ASSERT(found_content_type, "Should normalize content-type to Content-Type");
    TEST_ASSERT(found_content_length, "Should normalize CONTENT-LENGTH to Content-Length");
    TEST_ASSERT(found_accept_encoding, "Should normalize Accept-Encoding to Accept-Encoding");
    TEST_ASSERT(found_api_key, "Should normalize x-api-key to X-Api-Key");

    TEST_PASS("Client normalizes mixed case input");
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "Client Header Normalization Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_client_normalizes_to_canonical_case();
    test_client_no_fast_path_used();
    test_client_normalizes_mixed_case_input();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
