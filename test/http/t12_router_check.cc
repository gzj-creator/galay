/**
 * @file test_http_router_validation.cc
 * @brief HttpRouter 路径验证测试
 */

#include <iostream>
#include <cassert>
#include "galay-http/server/http_router.h"
#include "galay-http/protoc/http_request.h"

using namespace galay::http;
using namespace galay::kernel;

// 测试用的处理器
Task<void> test_handler(HttpConn& conn, HttpRequest req) {
    co_return;
}

void test_valid_paths() {

    HttpRouter router;

    // 有效的路径
    std::vector<std::string> validPaths = {
        "/",
        "/api",
        "/api/users",
        "/api/users/:id",
        "/api/users/:userId/posts/:postId",
        "/user/:id",
        "/static/*",
        "/files/**",
        "/api/v1/users",
        "/path-with-dash",
        "/path_with_underscore",
        "/path.with.dot",
        "/path~with~tilde",
        "/api/users/:user_id",
        "/api/users/:userId123"
    };

    size_t successCount = 0;
    for (const auto& path : validPaths) {
        size_t beforeSize = router.size();
        router.addHandler<HttpMethod::GET>(path, test_handler);
        size_t afterSize = router.size();

        if (afterSize > beforeSize) {
            successCount++;
        } else {
        }
    }

    assert(successCount == validPaths.size());
}

void test_invalid_paths() {

    HttpRouter router;

    // 无效的路径
    struct TestCase {
        std::string path;
        std::string reason;
    };

    std::vector<TestCase> invalidPaths = {
        {"", "Empty path"},
        {"api/users", "Missing leading /"},
        {"/api/users/:id/:id", "Duplicate parameter name"},
        {"/api/*/extra", "Wildcard not at end"},
        {"/api/**/extra", "Greedy wildcard not at end"},
        {"/api/:", "Empty parameter name"},
        {"/api/:user-id", "Invalid character in parameter name"},
        {"/api/:user id", "Space in parameter name"},
        {"/api/users/*/posts", "Wildcard not at end"},
        {"/api/users/**/**", "Multiple wildcards"},
        {"/api/users/:id/:name/:id", "Duplicate parameter name (3 params)"},
        {"/api/users/:123", "Parameter name starts with number"},
        {"/api/users/:user@id", "Invalid character @ in parameter"},
        {"/api/users/:user#id", "Invalid character # in parameter"}
    };

    size_t rejectedCount = 0;
    for (const auto& [path, reason] : invalidPaths) {
        size_t beforeSize = router.size();
        router.addHandler<HttpMethod::GET>(path, test_handler);
        size_t afterSize = router.size();

        if (afterSize == beforeSize) {
            rejectedCount++;
        } else {
        }
    }

    assert(rejectedCount == invalidPaths.size());
}

void test_duplicate_routes() {

    HttpRouter router;

    // 第一次添加
    router.addHandler<HttpMethod::GET>("/api/users", test_handler);
    size_t size1 = router.size();

    // 第二次添加相同路由（应该覆盖并警告）
    router.addHandler<HttpMethod::GET>("/api/users", test_handler);
    size_t size2 = router.size();

    // 大小应该相同（覆盖而不是新增）
    assert(size1 == size2);
}

void test_parameter_extraction() {

    HttpRouter router;

    // 注册带参数的路由
    router.addHandler<HttpMethod::GET>("/user/:id", test_handler);
    router.addHandler<HttpMethod::GET>("/user/:userId/posts/:postId", test_handler);

    // 测试参数提取
    auto match1 = router.findHandler(HttpMethod::GET, "/user/123");
    assert(match1.handler != nullptr);
    assert(match1.params.size() == 1);
    assert(match1.params["id"] == "123");

    auto match2 = router.findHandler(HttpMethod::GET, "/user/456/posts/789");
    assert(match2.handler != nullptr);
    assert(match2.params.size() == 2);
    assert(match2.params["userId"] == "456");
    assert(match2.params["postId"] == "789");

}

void test_edge_cases() {

    HttpRouter router;

    // 根路径
    router.addHandler<HttpMethod::GET>("/", test_handler);
    auto match1 = router.findHandler(HttpMethod::GET, "/");
    assert(match1.handler != nullptr);

    // 很长的路径（但在限制内）
    std::string longPath = "/api";
    for (int i = 0; i < 50; ++i) {
        longPath += "/segment" + std::to_string(i);
    }
    router.addHandler<HttpMethod::GET>(longPath, test_handler);
    auto match2 = router.findHandler(HttpMethod::GET, longPath);
    assert(match2.handler != nullptr);

    // 多个参数
    router.addHandler<HttpMethod::GET>("/a/:p1/b/:p2/c/:p3/d/:p4", test_handler);
    auto match3 = router.findHandler(HttpMethod::GET, "/a/1/b/2/c/3/d/4");
    assert(match3.handler != nullptr);
    assert(match3.params.size() == 4);

}

void test_http_request_integration() {

    HttpRouter router;
    router.addHandler<HttpMethod::GET>("/user/:id/posts/:postId", test_handler);

    // 模拟路由匹配和参数设置
    auto match = router.findHandler(HttpMethod::GET, "/user/123/posts/456");
    assert(match.handler != nullptr);

    // 创建 HttpRequest 并设置参数
    HttpRequest request;
    request.setRouteParams(std::move(match.params));

    // 验证参数可以从 HttpRequest 中获取
    assert(request.hasRouteParam("id"));
    assert(request.hasRouteParam("postId"));
    assert(request.getRouteParam("id") == "123");
    assert(request.getRouteParam("postId") == "456");
    assert(request.getRouteParam("nonexistent", "default") == "default");


}

int main() {

    try {
        test_valid_paths();
        test_invalid_paths();
        test_duplicate_routes();
        test_parameter_extraction();
        test_edge_cases();
        test_http_request_integration();

        return 0;
    } catch (const std::exception& e) {
        return 1;
    }
}
