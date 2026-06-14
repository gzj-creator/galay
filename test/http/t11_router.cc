/**
 * @file test_http_router.cc
 * @brief HttpRouter 单元测试
 */

#include <iostream>
#include <cassert>
#include <filesystem>
#include "http/server/http_router.h"
#include "http/protoc/http_request.h"
#include "http/protoc/http_response.h"

using namespace galay::http;

namespace {

std::string resolveStaticDir() {
    namespace fs = std::filesystem;

    const std::string candidates[] = {
        "./test/static_files",
        "./static_files",
        "../test/static_files",
        "../../test/static_files"
    };

    for (const auto& path : candidates) {
        if (fs::exists(path) && fs::is_directory(path)) {
            return path;
        }
    }

    return "./test/static_files";
}

} // namespace

// 测试用的简单处理器
galay::kernel::Task<void> testHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

galay::kernel::Task<void> userHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

galay::kernel::Task<void> postHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

galay::kernel::Task<void> staticHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

galay::kernel::Task<void> filesHandler(HttpConn& conn, HttpRequest req) {
    co_return;
}

void testExactMatch() {

    HttpRouter router;

    // 添加精确匹配路由
    router.addHandler<HttpMethod::GET>("/api/users", testHandler);
    router.addHandler<HttpMethod::POST>("/api/users", postHandler);
    router.addHandler<HttpMethod::GET>("/api/posts", testHandler);

    // 测试精确匹配
    auto match1 = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match1.handler != nullptr);
    assert(match1.params.empty());

    auto match2 = router.findHandler(HttpMethod::POST, "/api/users");
    assert(match2.handler != nullptr);

    auto match3 = router.findHandler(HttpMethod::GET, "/api/posts");
    assert(match3.handler != nullptr);

    // 测试不匹配
    auto match4 = router.findHandler(HttpMethod::GET, "/api/comments");
    assert(match4.handler == nullptr);

    auto match5 = router.findHandler(HttpMethod::DELETE, "/api/users");
    assert(match5.handler == nullptr);

}

void testPathParameters() {

    HttpRouter router;

    // 添加路径参数路由
    router.addHandler<HttpMethod::GET>("/user/:id", userHandler);
    router.addHandler<HttpMethod::GET>("/user/:id/posts/:postId", postHandler);

    // 测试单个参数
    auto match1 = router.findHandler(HttpMethod::GET, "/user/123");
    assert(match1.handler != nullptr);
    assert(match1.params.size() == 1);
    assert(match1.params["id"] == "123");

    auto match2 = router.findHandler(HttpMethod::GET, "/user/abc");
    assert(match2.handler != nullptr);
    assert(match2.params["id"] == "abc");

    // 测试多个参数
    auto match3 = router.findHandler(HttpMethod::GET, "/user/456/posts/789");
    assert(match3.handler != nullptr);
    assert(match3.params.size() == 2);
    assert(match3.params["id"] == "456");
    assert(match3.params["postId"] == "789");

    // 测试不匹配
    auto match4 = router.findHandler(HttpMethod::GET, "/user");
    assert(match4.handler == nullptr);

    auto match5 = router.findHandler(HttpMethod::GET, "/user/123/posts");
    assert(match5.handler == nullptr);

}

void testWildcard() {

    HttpRouter router;

    // 添加通配符路由
    router.addHandler<HttpMethod::GET>("/static/*", staticHandler);
    router.addHandler<HttpMethod::GET>("/files/**", filesHandler);

    // 测试单段通配符
    auto match1 = router.findHandler(HttpMethod::GET, "/static/css");
    assert(match1.handler != nullptr);

    auto match2 = router.findHandler(HttpMethod::GET, "/static/js");
    assert(match2.handler != nullptr);

    // 单段通配符不应匹配多段
    auto match3 = router.findHandler(HttpMethod::GET, "/static/css/style.css");
    assert(match3.handler == nullptr);

    // 测试贪婪通配符
    auto match4 = router.findHandler(HttpMethod::GET, "/files/a");
    assert(match4.handler != nullptr);

    auto match5 = router.findHandler(HttpMethod::GET, "/files/a/b/c");
    assert(match5.handler != nullptr);

}

void testMultipleMethods() {

    HttpRouter router;

    // 为同一路径添加多个方法
    router.addHandler<HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT>("/api/resource", testHandler);

    auto match1 = router.findHandler(HttpMethod::GET, "/api/resource");
    assert(match1.handler != nullptr);

    auto match2 = router.findHandler(HttpMethod::POST, "/api/resource");
    assert(match2.handler != nullptr);

    auto match3 = router.findHandler(HttpMethod::PUT, "/api/resource");
    assert(match3.handler != nullptr);

    auto match4 = router.findHandler(HttpMethod::DELETE, "/api/resource");
    assert(match4.handler == nullptr);

}

void testPriorityMatching() {

    HttpRouter router;

    // 添加不同优先级的路由
    router.addHandler<HttpMethod::GET>("/api/users", testHandler);      // 精确匹配
    router.addHandler<HttpMethod::GET>("/api/:resource", userHandler);  // 参数匹配
    router.addHandler<HttpMethod::GET>("/api/*", staticHandler);        // 通配符匹配

    // 精确匹配应该优先
    auto match1 = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match1.handler != nullptr);
    assert(match1.params.empty());

    // 参数匹配应该次之
    auto match2 = router.findHandler(HttpMethod::GET, "/api/posts");
    assert(match2.handler != nullptr);
    assert(match2.params.size() == 1);
    assert(match2.params["resource"] == "posts");

}

void testRouterOperations() {

    HttpRouter router;

    // 测试 size
    assert(router.size() == 0);

    router.addHandler<HttpMethod::GET>("/api/users", testHandler);
    router.addHandler<HttpMethod::POST>("/api/users", postHandler);
    router.addHandler<HttpMethod::GET>("/user/:id", userHandler);

    assert(router.size() == 3);

    // 测试 remove
    bool removed = router.delHandler(HttpMethod::GET, "/api/users");
    assert(removed);
    assert(router.size() == 2);

    auto match = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match.handler == nullptr);

    // 测试 clear
    router.clear();
    assert(router.size() == 0);

    match = router.findHandler(HttpMethod::POST, "/api/users");
    assert(match.handler == nullptr);

}

void testEdgeCases() {

    HttpRouter router;

    // 根路径
    router.addHandler<HttpMethod::GET>("/", testHandler);
    auto match1 = router.findHandler(HttpMethod::GET, "/");
    assert(match1.handler != nullptr);

    // 带尾部斜杠的路径
    router.addHandler<HttpMethod::GET>("/api/users/", userHandler);
    auto match2 = router.findHandler(HttpMethod::GET, "/api/users/");
    assert(match2.handler != nullptr);

    // 空段应该被忽略
    auto match3 = router.findHandler(HttpMethod::GET, "//api//users//");
    // 这取决于 splitPath 的实现，应该与 /api/users/ 相同

}

void testProxyMounting() {

    HttpRouter router;

    router.proxy("/api", "127.0.0.1", 8080);

    auto match1 = router.findHandler(HttpMethod::GET, "/api");
    assert(match1.handler != nullptr);

    auto match2 = router.findHandler(HttpMethod::GET, "/api/users");
    assert(match2.handler != nullptr);

    auto match3 = router.findHandler(HttpMethod::POST, "/api/orders");
    assert(match3.handler != nullptr);

    auto match4 = router.findHandler(HttpMethod::GET, "/other/path");
    assert(match4.handler == nullptr);

    HttpRouter root_proxy_router;
    root_proxy_router.proxy("/", "127.0.0.1", 8080);
    auto match5 = root_proxy_router.findHandler(HttpMethod::GET, "/any/path");
    assert(match5.handler != nullptr);

}

void testTryFilesMounting() {

    HttpRouter router;
    router.tryFiles("/static", resolveStaticDir(), "127.0.0.1", 8080);

    auto match1 = router.findHandler(HttpMethod::GET, "/static/index.html");
    assert(match1.handler != nullptr);

    auto match2 = router.findHandler(HttpMethod::HEAD, "/static/does-not-exist.txt");
    assert(match2.handler != nullptr);

    auto match3 = router.findHandler(HttpMethod::GET, "/other/path");
    assert(match3.handler == nullptr);

}

void testFallbackProxyConfig() {

    HttpRouter router;
    router.proxy("/", "127.0.0.1", 8080, ProxyMode::Http);
    auto match = router.findHandler(HttpMethod::GET, "/any/path");
    assert(match.handler != nullptr);

}

int main() {

    try {
        testExactMatch();
        testPathParameters();
        testWildcard();
        testMultipleMethods();
        testPriorityMatching();
        testRouterOperations();
        testEdgeCases();
        testProxyMounting();
        testTryFilesMounting();
        testFallbackProxyConfig();

        return 0;
    } catch (const std::exception& e) {
        return 1;
    }
}
