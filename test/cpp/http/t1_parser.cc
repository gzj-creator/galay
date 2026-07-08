/**
 * @file test_http_parser.cc
 * @brief HTTP Request/Response 增量解析测试
 *
 * 测试场景：
 * 1. 头部一次不全
 * 2. body一次不全
 * 3. RingBuffer包含多个完整Request/Response
 * 4. RingBuffer包含完整和不完整的
 * 5. 配合最大头长度限制
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <galay/cpp/galay-http/common/iovec_utils.h>
#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-http/protoc/http_response.h>
#include <galay/cpp/galay-http/protoc/http_error.h>
#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

using namespace galay::http;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;

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

static std::vector<struct iovec> readIovecs(const RingBuffer<>& buffer)
{
    auto borrowed = borrowReadIovecs(buffer);
    std::vector<struct iovec> iovecs;
    IoVecWindow::buildWindow(borrowed, iovecs);
    return iovecs;
}

// ============ HttpRequest 测试 ============

void test_request_complete_in_one_shot()
{
    std::cout << "\n=== Test: Request complete in one shot ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string raw = "GET /index.html HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "hello";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()), "Should consume all bytes");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().uri() == "/index.html", "URI should match");
    TEST_ASSERT(request.header().method() == HttpMethod::GET, "Method should be GET");
    TEST_ASSERT(request.getBodyStr() == "hello", "Body should match");

    buffer.consume(consumed);
    TEST_ASSERT(buffer.readable() == 0, "Buffer should be empty");

    TEST_PASS("Request complete in one shot");
}

void test_request_header_partial()
{
    std::cout << "\n=== Test: Request header partial ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    // 第一部分：不完整的header
    std::string part1 = "GET /api/test HTTP/1.1\r\n"
                        "Host: example";
    buffer.write(part1);

    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request.fromIOVec(iovecs1);

    TEST_ASSERT(err1 == kNoError, "Should not error on partial header");
    TEST_ASSERT(consumed1 == static_cast<ssize_t>(part1.length()), "Should consume partial header bytes");
    TEST_ASSERT(!request.isComplete(), "Request should not be complete");

    buffer.consume(consumed1);

    // 第二部分：完成header
    std::string part2 = ".com\r\n"
                        "Content-Length: 0\r\n"
                        "\r\n";
    buffer.write(part2);

    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request.fromIOVec(iovecs2);

    TEST_ASSERT(err2 == kNoError, "Should parse without error");
    TEST_ASSERT(consumed2 > 0, "Should consume bytes");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().headerPairs().getValue("Host") == "example.com", "Host should match");

    buffer.consume(consumed2);

    TEST_PASS("Request header partial");
}

void test_request_body_partial()
{
    std::cout << "\n=== Test: Request body partial ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    // 完整header + 部分body
    std::string part1 = "POST /upload HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 20\r\n"
                        "\r\n"
                        "12345";  // 只有5字节，需要20字节
    buffer.write(part1);

    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request.fromIOVec(iovecs1);

    TEST_ASSERT(err1 == kNoError, "Should not error on partial body");
    TEST_ASSERT(consumed1 == static_cast<ssize_t>(part1.length()), "Should return consumed bytes for partial data");
    TEST_ASSERT(!request.isComplete(), "Request should not be complete");

    buffer.consume(consumed1);  // 消费已解析的数据

    // 继续写入body
    std::string part2 = "67890abcde";  // 再10字节
    buffer.write(part2);

    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request.fromIOVec(iovecs2);

    TEST_ASSERT(err2 == kNoError, "Should not error");
    TEST_ASSERT(consumed2 == static_cast<ssize_t>(part2.length()), "Should return consumed bytes for additional data");
    TEST_ASSERT(!request.isComplete(), "Request should still not be complete");

    buffer.consume(consumed2);  // 消费已解析的数据

    // 写入剩余body
    std::string part3 = "fghij";  // 最后5字节
    buffer.write(part3);

    auto iovecs3 = readIovecs(buffer);
    auto [err3, consumed3] = request.fromIOVec(iovecs3);

    TEST_ASSERT(err3 == kNoError, "Should parse without error");
    TEST_ASSERT(consumed3 > 0, "Should consume bytes");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.getBodyStr() == "1234567890abcdefghij", "Body should match");

    buffer.consume(consumed3);

    TEST_PASS("Request body partial");
}

void test_request_multiple_complete()
{
    std::cout << "\n=== Test: Multiple complete requests in buffer ===" << std::endl;

    RingBuffer buffer(8192);

    std::string req1 = "GET /page1 HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    std::string req2 = "GET /page2 HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    std::string req3 = "POST /api HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 4\r\n"
                       "\r\n"
                       "test";

    buffer.write(req1);
    buffer.write(req2);
    buffer.write(req3);

    // 解析第一个请求
    HttpRequest request1;
    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request1.fromIOVec(iovecs1);

    TEST_ASSERT(err1 == kNoError, "Should parse first request");
    TEST_ASSERT(request1.isComplete(), "First request should be complete");
    TEST_ASSERT(request1.header().uri() == "/page1", "First URI should match");

    buffer.consume(consumed1);

    // 解析第二个请求
    HttpRequest request2;
    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request2.fromIOVec(iovecs2);

    TEST_ASSERT(err2 == kNoError, "Should parse second request");
    TEST_ASSERT(request2.isComplete(), "Second request should be complete");
    TEST_ASSERT(request2.header().uri() == "/page2", "Second URI should match");

    buffer.consume(consumed2);

    // 解析第三个请求
    HttpRequest request3;
    auto iovecs3 = readIovecs(buffer);
    auto [err3, consumed3] = request3.fromIOVec(iovecs3);

    TEST_ASSERT(err3 == kNoError, "Should parse third request");
    TEST_ASSERT(request3.isComplete(), "Third request should be complete");
    TEST_ASSERT(request3.header().uri() == "/api", "Third URI should match");
    TEST_ASSERT(request3.header().method() == HttpMethod::POST, "Third method should be POST");
    TEST_ASSERT(request3.getBodyStr() == "test", "Third body should match");

    buffer.consume(consumed3);
    TEST_ASSERT(buffer.readable() == 0, "Buffer should be empty");

    TEST_PASS("Multiple complete requests in buffer");
}

void test_request_complete_and_partial()
{
    std::cout << "\n=== Test: Complete request followed by partial ===" << std::endl;

    RingBuffer buffer(4096);

    std::string complete_req = "GET /complete HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "\r\n";
    std::string partial_req = "POST /partial HTTP/1.1\r\n"
                              "Host: local";  // 不完整

    buffer.write(complete_req);
    buffer.write(partial_req);

    // 解析第一个完整请求
    HttpRequest request1;
    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request1.fromIOVec(iovecs1);

    TEST_ASSERT(err1 == kNoError, "Should parse first request");
    TEST_ASSERT(request1.isComplete(), "First request should be complete");
    TEST_ASSERT(request1.header().uri() == "/complete", "First URI should match");

    buffer.consume(consumed1);

    // 尝试解析第二个不完整请求
    HttpRequest request2;
    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request2.fromIOVec(iovecs2);

    TEST_ASSERT(err2 == kNoError, "Should not error on partial");
    TEST_ASSERT(consumed2 == static_cast<ssize_t>(partial_req.length()), "Should consume partial bytes");
    TEST_ASSERT(!request2.isComplete(), "Second request should not be complete");

    buffer.consume(consumed2);

    // 完成第二个请求
    std::string rest = "host\r\n"
                       "Content-Length: 0\r\n"
                       "\r\n";
    buffer.write(rest);

    auto iovecs3 = readIovecs(buffer);
    auto [err3, consumed3] = request2.fromIOVec(iovecs3);

    TEST_ASSERT(err3 == kNoError, "Should parse completed request");
    TEST_ASSERT(request2.isComplete(), "Second request should now be complete");
    TEST_ASSERT(request2.header().uri() == "/partial", "Second URI should match");

    buffer.consume(consumed3);

    TEST_PASS("Complete request followed by partial");
}

void test_request_no_body()
{
    std::cout << "\n=== Test: Request without body ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string raw = "GET /index.html HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()), "Should consume all bytes");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().isKeepAlive(), "Should be keep-alive");

    TEST_PASS("Request without body");
}

void test_request_with_query_params()
{
    std::cout << "\n=== Test: Request with query parameters ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string raw = "GET /search?q=hello&page=1 HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().uri() == "/search", "URI should be path only");
    TEST_ASSERT(request.header().args()["q"] == "hello", "Query param q should match");
    TEST_ASSERT(request.header().args()["page"] == "1", "Query param page should match");

    TEST_PASS("Request with query parameters");
}

void test_request_reset()
{
    std::cout << "\n=== Test: Request reset and reuse ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    // 第一个请求
    std::string req1 = "GET /first HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    buffer.write(req1);

    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request.fromIOVec(iovecs1);

    TEST_ASSERT(request.isComplete(), "First request should be complete");
    TEST_ASSERT(request.header().uri() == "/first", "First URI should match");

    buffer.consume(consumed1);

    // 重置并解析第二个请求
    request.reset();

    std::string req2 = "POST /second HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Length: 3\r\n"
                       "\r\n"
                       "abc";
    buffer.write(req2);

    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request.fromIOVec(iovecs2);

    TEST_ASSERT(request.isComplete(), "Second request should be complete");
    TEST_ASSERT(request.header().uri() == "/second", "Second URI should match");
    TEST_ASSERT(request.header().method() == HttpMethod::POST, "Method should be POST");
    TEST_ASSERT(request.getBodyStr() == "abc", "Body should match");

    TEST_PASS("Request reset and reuse");
}

// ============ HttpResponse 测试 ============

void test_response_complete_in_one_shot()
{
    std::cout << "\n=== Test: Response complete in one shot ===" << std::endl;

    RingBuffer buffer(4096);
    HttpResponse response;

    std::string raw = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: 13\r\n"
                      "\r\n"
                      "Hello, World!";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = response.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()), "Should consume all bytes");
    TEST_ASSERT(response.isComplete(), "Response should be complete");
    TEST_ASSERT(response.header().code() == HttpStatusCode::OK_200, "Status should be 200");
    TEST_ASSERT(response.getBodyStr() == "Hello, World!", "Body should match");

    TEST_PASS("Response complete in one shot");
}

void test_response_header_partial()
{
    std::cout << "\n=== Test: Response header partial ===" << std::endl;

    RingBuffer buffer(4096);
    HttpResponse response;

    // 第一部分
    std::string part1 = "HTTP/1.1 404 Not Found\r\n"
                        "Content-Type: text/";
    buffer.write(part1);

    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = response.fromIOVec(iovecs1);

    TEST_ASSERT(err1 == kNoError, "Should not error on partial");
    TEST_ASSERT(consumed1 == static_cast<ssize_t>(part1.length()), "Should consume partial bytes");
    TEST_ASSERT(!response.isComplete(), "Response should not be complete");

    buffer.consume(consumed1);

    // 第二部分
    std::string part2 = "plain\r\n"
                        "Content-Length: 9\r\n"
                        "\r\n"
                        "Not Found";
    buffer.write(part2);

    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = response.fromIOVec(iovecs2);

    TEST_ASSERT(err2 == kNoError, "Should parse without error");
    TEST_ASSERT(response.isComplete(), "Response should be complete");
    TEST_ASSERT(response.header().code() == HttpStatusCode::NotFound_404, "Status should be 404");

    TEST_PASS("Response header partial");
}

void test_response_body_partial()
{
    std::cout << "\n=== Test: Response body partial ===" << std::endl;

    RingBuffer buffer(4096);
    HttpResponse response;

    std::string part1 = "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 100\r\n"
                        "\r\n"
                        "0123456789";  // 只有10字节
    buffer.write(part1);

    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = response.fromIOVec(iovecs1);

    TEST_ASSERT(err1 == kNoError, "Should not error");
    TEST_ASSERT(consumed1 == static_cast<ssize_t>(part1.length()), "Should return consumed bytes for partial data");
    TEST_ASSERT(!response.isComplete(), "Response should not be complete");

    buffer.consume(consumed1);  // 消费已解析的数据

    // 写入更多数据
    std::string part2(90, 'x');  // 剩余90字节
    buffer.write(part2);

    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = response.fromIOVec(iovecs2);

    TEST_ASSERT(err2 == kNoError, "Should parse without error");
    TEST_ASSERT(consumed2 == 90, "Should consume remaining body bytes");
    TEST_ASSERT(response.isComplete(), "Response should be complete");

    std::string expected_body = "0123456789" + std::string(90, 'x');
    TEST_ASSERT(response.getBodyStr() == expected_body, "Body should match");

    TEST_PASS("Response body partial");
}

void test_response_multiple_complete()
{
    std::cout << "\n=== Test: Multiple complete responses in buffer ===" << std::endl;

    RingBuffer buffer(8192);

    std::string resp1 = "HTTP/1.1 200 OK\r\n"
                        "Content-Length: 2\r\n"
                        "\r\n"
                        "OK";
    std::string resp2 = "HTTP/1.1 201 Created\r\n"
                        "Content-Length: 7\r\n"
                        "\r\n"
                        "Created";
    std::string resp3 = "HTTP/1.1 204 No Content\r\n"
                        "\r\n";

    buffer.write(resp1);
    buffer.write(resp2);
    buffer.write(resp3);

    // 解析第一个响应
    HttpResponse response1;
    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = response1.fromIOVec(iovecs1);

    TEST_ASSERT(response1.isComplete(), "First response should be complete");
    TEST_ASSERT(response1.header().code() == HttpStatusCode::OK_200, "First status should be 200");

    buffer.consume(consumed1);

    // 解析第二个响应
    HttpResponse response2;
    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = response2.fromIOVec(iovecs2);

    TEST_ASSERT(response2.isComplete(), "Second response should be complete");
    TEST_ASSERT(response2.header().code() == HttpStatusCode::Created_201, "Second status should be 201");

    buffer.consume(consumed2);

    // 解析第三个响应
    HttpResponse response3;
    auto iovecs3 = readIovecs(buffer);
    auto [err3, consumed3] = response3.fromIOVec(iovecs3);

    TEST_ASSERT(response3.isComplete(), "Third response should be complete");
    TEST_ASSERT(response3.header().code() == HttpStatusCode::NoContent_204, "Third status should be 204");

    buffer.consume(consumed3);
    TEST_ASSERT(buffer.readable() == 0, "Buffer should be empty");

    TEST_PASS("Multiple complete responses in buffer");
}

void test_response_no_status_text()
{
    std::cout << "\n=== Test: Response without status text ===" << std::endl;

    RingBuffer buffer(4096);
    HttpResponse response;

    // 有些服务器可能不发送状态文本
    std::string raw = "HTTP/1.1 200\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = response.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(response.isComplete(), "Response should be complete");
    TEST_ASSERT(response.header().code() == HttpStatusCode::OK_200, "Status should be 200");

    TEST_PASS("Response without status text");
}

// ============ 错误处理测试 ============

void test_request_bad_format()
{
    std::cout << "\n=== Test: Request bad format ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    // 缺少HTTP版本
    std::string raw = "GET /index.html\r\n"
                      "Host: localhost\r\n"
                      "\r\n";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kBadRequest, "Should return bad request error");
    TEST_ASSERT(consumed == -1, "Should return -1 on error");

    TEST_PASS("Request bad format");
}

void test_response_invalid_status_code()
{
    std::cout << "\n=== Test: Response invalid status code ===" << std::endl;

    RingBuffer buffer(4096);
    HttpResponse response;

    std::string raw = "HTTP/1.1 abc OK\r\n"
                      "\r\n";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = response.fromIOVec(iovecs);

    TEST_ASSERT(err == kHttpCodeInvalid, "Should return invalid code error");
    TEST_ASSERT(consumed == -1, "Should return -1 on error");

    TEST_PASS("Response invalid status code");
}

void test_request_unsupported_version()
{
    std::cout << "\n=== Test: Request unsupported HTTP version ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string raw = "GET /index.html HTTP/3.0\r\n"
                      "Host: localhost\r\n"
                      "\r\n";

    buffer.write(raw);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kVersionNotSupport, "Should return version not support error");

    TEST_PASS("Request unsupported HTTP version");
}

// ============ RingBuffer 环绕测试 ============

void test_ringbuffer_wrap_around()
{
    std::cout << "\n=== Test: RingBuffer wrap around ===" << std::endl;

    // 使用小容量buffer测试环绕
    RingBuffer buffer(128);

    // 先写入一些数据并消费，让写指针前进
    std::string filler(100, 'x');
    buffer.write(filler);
    buffer.consume(100);

    // 现在写入会环绕
    std::string req = "GET /wrap HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";
    buffer.write(req);

    HttpRequest request;
    auto iovecs = readIovecs(buffer);

    // 验证iovec数量（环绕时应该是2个）
    TEST_ASSERT(iovecs.size() >= 1, "Should have at least 1 iovec");

    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().uri() == "/wrap", "URI should match");

    TEST_PASS("RingBuffer wrap around");
}

void test_ringbuffer_header_split_across_wrap()
{
    std::cout << "\n=== Test: Header split across RingBuffer wrap ===" << std::endl;

    RingBuffer buffer(128);

    // 填充buffer让写指针接近末尾
    std::string filler(110, 'x');
    buffer.write(filler);
    buffer.consume(110);

    // 写入请求，header会跨越环绕边界
    std::string req = "GET /split HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "User-Agent: TestAgent\r\n"
                      "\r\n";
    buffer.write(req);

    HttpRequest request;
    auto iovecs = readIovecs(buffer);

    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().uri() == "/split", "URI should match");
    TEST_ASSERT(request.header().headerPairs().getValue("User-Agent") == "TestAgent", "User-Agent should match");

    TEST_PASS("Header split across RingBuffer wrap");
}

void test_ringbuffer_body_split_across_wrap()
{
    std::cout << "\n=== Test: Body split across RingBuffer wrap ===" << std::endl;

    RingBuffer buffer(256);

    // 填充buffer
    std::string filler(200, 'x');
    buffer.write(filler);
    buffer.consume(200);

    // 写入请求，body会跨越环绕边界
    std::string header = "POST /data HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 50\r\n"
                        "\r\n";
    std::string body(50, 'B');

    buffer.write(header);
    buffer.write(body);

    HttpRequest request;
    auto iovecs = readIovecs(buffer);

    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.getBodyStr() == body, "Body should match");

    TEST_PASS("Body split across RingBuffer wrap");
}

// ============ 边界情况测试 ============

void test_header_exactly_at_boundary()
{
    std::cout << "\n=== Test: Header ends exactly at buffer boundary ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    // 构造一个header刚好在某个边界结束的情况
    std::string req = "GET /boundary HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "\r\n";

    buffer.write(req);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");

    TEST_PASS("Header ends exactly at buffer boundary");
}

void test_body_exactly_content_length()
{
    std::cout << "\n=== Test: Body exactly matches Content-Length ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string body = "ExactBody";
    std::string req = "POST /exact HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    buffer.write(req);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.getBodyStr() == body, "Body should match exactly");

    TEST_PASS("Body exactly matches Content-Length");
}

void test_incremental_single_byte()
{
    std::cout << "\n=== Test: Incremental parsing one byte at a time ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string req = "GET /slow HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 5\r\n"
                      "\r\n"
                      "12345";

    // 一次写入一个字节
    for (size_t i = 0; i < req.size(); ++i) {
        buffer.write(std::string(1, req[i]));

        auto iovecs = readIovecs(buffer);
        auto [err, consumed] = request.fromIOVec(iovecs);

        TEST_ASSERT(err == kNoError, "Should not error during incremental parse");
        if (consumed > 0) {
            buffer.consume(consumed);
        }
    }

    TEST_ASSERT(request.isComplete(), "Request should be complete");
    std::string body = request.getBodyStr();
    std::cout << "Body: [" << body << "] size: " << body.size() << std::endl;
    TEST_ASSERT(body == "12345", "Body should match");

    TEST_PASS("Incremental parsing one byte at a time");
}

void test_large_body()
{
    std::cout << "\n=== Test: Large body parsing ===" << std::endl;

    RingBuffer buffer(65536);
    HttpRequest request;

    std::string body(10000, 'L');
    std::string req = "POST /large HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    buffer.write(req);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.getBodyStr().size() == 10000, "Body size should match");

    TEST_PASS("Large body parsing");
}

void test_empty_header_value()
{
    std::cout << "\n=== Test: Empty header value ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string req = "GET /empty HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "X-Empty: \r\n"
                      "\r\n";

    buffer.write(req);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.header().headerPairs().getValue("X-Empty") == "", "Empty header should be empty string");

    TEST_PASS("Empty header value");
}

void test_multiple_requests_with_partial_last()
{
    std::cout << "\n=== Test: Multiple complete + partial last request ===" << std::endl;

    RingBuffer buffer(8192);

    std::string req1 = "GET /first HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    std::string req2 = "GET /second HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "\r\n";
    std::string req3_partial = "POST /third HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "Content-Length: 100\r\n"
                               "\r\n"
                               "partial";  // 只有部分body

    buffer.write(req1);
    buffer.write(req2);
    buffer.write(req3_partial);

    // 解析第一个
    HttpRequest request1;
    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request1.fromIOVec(iovecs1);
    TEST_ASSERT(request1.isComplete(), "First should be complete");
    buffer.consume(consumed1);

    // 解析第二个
    HttpRequest request2;
    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request2.fromIOVec(iovecs2);
    TEST_ASSERT(request2.isComplete(), "Second should be complete");
    buffer.consume(consumed2);

    // 解析第三个（不完整）
    HttpRequest request3;
    auto iovecs3 = readIovecs(buffer);
    auto [err3, consumed3] = request3.fromIOVec(iovecs3);
    TEST_ASSERT(err3 == kNoError, "Should not error");
    TEST_ASSERT(consumed3 == static_cast<ssize_t>(req3_partial.length()), "Should return consumed bytes for partial request");
    TEST_ASSERT(!request3.isComplete(), "Third should not be complete");

    buffer.consume(consumed3);  // 消费已解析的数据

    // 补充剩余数据
    std::string remaining(93, 'X');
    buffer.write(remaining);

    auto iovecs4 = readIovecs(buffer);
    auto [err4, consumed4] = request3.fromIOVec(iovecs4);
    TEST_ASSERT(request3.isComplete(), "Third should now be complete");

    TEST_PASS("Multiple complete + partial last request");
}

void test_zero_content_length()
{
    std::cout << "\n=== Test: Zero Content-Length ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string req = "POST /zero HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n";

    buffer.write(req);

    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.getBodyStr().empty(), "Body should be empty");

    TEST_PASS("Zero Content-Length");
}

void test_header_split_in_middle_of_crlf()
{
    std::cout << "\n=== Test: Header split in middle of CRLF ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    // 第一部分：在\r处断开
    std::string part1 = "GET /crlf HTTP/1.1\r\n"
                        "Host: localhost\r";
    buffer.write(part1);

    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request.fromIOVec(iovecs1);
    TEST_ASSERT(consumed1 == static_cast<ssize_t>(part1.length()), "Should consume partial bytes");

    buffer.consume(consumed1);

    // 第二部分：补充\n和剩余
    std::string part2 = "\n"
                        "\r\n";
    buffer.write(part2);

    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request.fromIOVec(iovecs2);
    TEST_ASSERT(request.isComplete(), "Request should be complete");

    TEST_PASS("Header split in middle of CRLF");
}

void test_body_split_multiple_times()
{
    std::cout << "\n=== Test: Body split multiple times ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string header = "POST /split HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: 30\r\n"
                        "\r\n";
    buffer.write(header);

    // 分多次写入body
    buffer.write("12345");  // 5字节
    auto iovecs1 = readIovecs(buffer);
    auto [err1, consumed1] = request.fromIOVec(iovecs1);
    TEST_ASSERT(!request.isComplete(), "Should not be complete");
    buffer.consume(consumed1);  // 消费已解析的数据

    buffer.write("67890");  // 再5字节
    auto iovecs2 = readIovecs(buffer);
    auto [err2, consumed2] = request.fromIOVec(iovecs2);
    TEST_ASSERT(!request.isComplete(), "Should not be complete");
    buffer.consume(consumed2);  // 消费已解析的数据

    buffer.write("abcdefghij");  // 再10字节
    auto iovecs3 = readIovecs(buffer);
    auto [err3, consumed3] = request.fromIOVec(iovecs3);
    TEST_ASSERT(!request.isComplete(), "Should not be complete");
    buffer.consume(consumed3);  // 消费已解析的数据

    buffer.write("klmnopqrst");  // 最后10字节
    auto iovecs4 = readIovecs(buffer);
    auto [err4, consumed4] = request.fromIOVec(iovecs4);
    TEST_ASSERT(request.isComplete(), "Should be complete");
    TEST_ASSERT(request.getBodyStr() == "1234567890abcdefghijklmnopqrst", "Body should match");

    TEST_PASS("Body split multiple times");
}

void test_request_lowercase_content_length()
{
    std::cout << "\n=== Test: Request lowercase content-length ===" << std::endl;

    RingBuffer buffer(4096);
    HttpRequest request;

    std::string body = "{\"k\":\"v\"}";
    std::string raw = "POST /lower HTTP/1.1\r\n"
                      "host: localhost\r\n"
                      "content-type: application/json\r\n"
                      "content-length: " + std::to_string(body.size()) + "\r\n"
                      "\r\n" + body;

    buffer.write(raw);
    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = request.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()), "Should consume full request");
    TEST_ASSERT(request.isComplete(), "Request should be complete");
    TEST_ASSERT(request.getBodyStr() == body, "Body should match");

    TEST_PASS("Request lowercase content-length");
}

void test_response_chunked_case_insensitive()
{
    std::cout << "\n=== Test: Response chunked case-insensitive ===" << std::endl;

    RingBuffer buffer(4096);
    HttpResponse response;

    std::string raw = "HTTP/1.1 200 OK\r\n"
                      "transfer-encoding: Chunked\r\n"
                      "connection: keep-alive\r\n"
                      "\r\n"
                      "5\r\nhello\r\n"
                      "0\r\n\r\n";

    buffer.write(raw);
    auto iovecs = readIovecs(buffer);
    auto [err, consumed] = response.fromIOVec(iovecs);

    TEST_ASSERT(err == kNoError, "Should parse without error");
    TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()), "Should consume full response");
    TEST_ASSERT(response.isComplete(), "Response should be complete");
    TEST_ASSERT(response.getBodyStr() == "hello", "Body should match");

    TEST_PASS("Response chunked case-insensitive");
}

void test_header_case_config_switch()
{
    std::cout << "\n=== Test: Header Mode ===" << std::endl;

    // ServerSide 模式：存 hOsT → 内部存为 host，查 host 能找到
    {
        HeaderPair headers(HeaderPair::Mode::ServerSide);
        headers.addHeaderPair("hOsT", "example.com");

        TEST_ASSERT(headers.getValue("host") == "example.com",
                    "ServerSide mode: lookup by 'host' should succeed");
        TEST_ASSERT(headers.getValue("HOST") == "example.com",
                    "ServerSide mode: lookup by 'HOST' should succeed (normalized to lowercase)");

        std::string str = headers.toString();
        TEST_ASSERT(str.find("host: example.com\r\n") != std::string::npos,
                    "ServerSide mode: stored key should be lowercase");
    }

    // ClientSide 模式：存 hOsT → 内部存为 Host，查 Host 能找到
    {
        HeaderPair headers(HeaderPair::Mode::ClientSide);
        headers.addHeaderPair("hOsT", "example.com");

        TEST_ASSERT(headers.getValue("Host") == "example.com",
                    "ClientSide mode: lookup by 'Host' should succeed");
        TEST_ASSERT(headers.getValue("host") == "example.com",
                    "ClientSide mode: lookup by 'host' should succeed (normalized to canonical)");

        std::string str = headers.toString();
        TEST_ASSERT(str.find("Host: example.com\r\n") != std::string::npos,
                    "ClientSide mode: stored key should be canonical");
    }

    // 解析测试：默认 Lowercase 模式下解析 HTTP 请求
    {
        HttpRequest request;
        RingBuffer buffer(1024);
        std::string raw = "GET /cfg HTTP/1.1\r\n"
                          "hOsT: example.com\r\n"
                          "\r\n";
        buffer.write(raw);
        auto [err, consumed] = request.fromIOVec(readIovecs(buffer));

        TEST_ASSERT(err == kNoError, "Parse should succeed");
        TEST_ASSERT(consumed == static_cast<ssize_t>(raw.size()), "Should consume full request");
        TEST_ASSERT(request.header().headerPairs().getValue("host") == "example.com",
                    "Default lowercase mode: lookup by 'host' should succeed");
    }

    TEST_PASS("Header Mode");
}

void test_header_add_normalized_pair()
{
    std::cout << "\n=== Test: Header add normalized pair ===" << std::endl;

    HeaderPair headers(HeaderPair::Mode::ServerSide);

    std::string key1 = "content-length";
    std::string value1 = "3";
    auto err1 = headers.addNormalizedHeaderPair(std::move(key1), std::move(value1));
    TEST_ASSERT(err1 == kNoError, "Insert normalized key should succeed");
    TEST_ASSERT(headers.getValue("Content-Length") == "3",
                "Lookup by mixed case should succeed for normalized insert");

    std::string key2 = "content-length";
    std::string value2 = "7";
    auto err2 = headers.addNormalizedHeaderPair(std::move(key2), std::move(value2));
    TEST_ASSERT(err2 == kNoError, "Update normalized key should succeed");
    TEST_ASSERT(headers.getValue("content-length") == "7",
                "Second normalized insert should update existing value");

    TEST_PASS("Header add normalized pair");
}

// ============ 主函数 ============

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "HTTP Parser Incremental Parsing Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    // HttpRequest 测试
    test_request_complete_in_one_shot();
    test_request_header_partial();
    test_request_body_partial();
    test_request_multiple_complete();
    test_request_complete_and_partial();
    test_request_no_body();
    test_request_with_query_params();
    test_request_reset();

    // HttpResponse 测试
    test_response_complete_in_one_shot();
    test_response_header_partial();
    test_response_body_partial();
    test_response_multiple_complete();
    test_response_no_status_text();

    // 错误处理测试
    test_request_bad_format();
    test_response_invalid_status_code();
    test_request_unsupported_version();

    // RingBuffer 环绕测试
    test_ringbuffer_wrap_around();
    test_ringbuffer_header_split_across_wrap();
    test_ringbuffer_body_split_across_wrap();

    // 边界情况测试
    test_header_exactly_at_boundary();
    test_body_exactly_content_length();
    test_incremental_single_byte();
    test_large_body();
    test_empty_header_value();
    test_multiple_requests_with_partial_last();
    test_zero_content_length();
    test_header_split_in_middle_of_crlf();
    test_body_split_multiple_times();
    test_request_lowercase_content_length();
    test_response_chunked_case_insensitive();
    test_header_case_config_switch();
    test_header_add_normalized_pair();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << g_passed << " passed, " << g_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return g_failed > 0 ? 1 : 0;
}
