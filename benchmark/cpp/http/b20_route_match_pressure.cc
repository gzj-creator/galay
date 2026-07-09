/**
 * @file b20_route_match_pressure.cc
 * @brief HTTP 动态路由匹配压力基准。
 *
 * 使用方法:
 *   ./benchmark_http_route_match_pressure [iterations]
 */

#include <galay/cpp/galay-http/server/http_router.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

using namespace galay::http;

namespace {

galay::kernel::Task<void> routeHandler(HttpConn& conn, HttpRequest request)
{
    co_return;
}

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_http_route_match_pressure] " << message << "\n";
        return false;
    }
    return true;
}

HttpRouter makeRouter()
{
    HttpRouter router;
    router.addHandler<HttpMethod::GET>("/api/users/:id/posts/:postId/comments/:commentId",
                                       routeHandler);
    router.addHandler<HttpMethod::GET>("/api/users/:id/profile", routeHandler);
    router.addHandler<HttpMethod::GET>("/assets/*/bundle", routeHandler);
    router.addHandler<HttpMethod::GET>("/files/**", routeHandler);
    return router;
}

template <typename Func>
bool runBench(const char* name, size_t iterations, Func&& func)
{
    size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        checksum += func(i);
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (!require(checksum != 0, "checksum should not be zero")) {
        return false;
    }
    std::cout << name << ": " << iterations << " iterations, "
              << (static_cast<double>(iterations) / seconds)
              << " ops/s, checksum=" << checksum << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    size_t iterations = 1000000;
    if (argc > 1) {
        const long requested = std::strtol(argv[1], nullptr, 10);
        if (requested > 0) {
            iterations = static_cast<size_t>(requested);
        }
    }

    HttpRouter router = makeRouter();
    const std::array<std::string, 4> paths = {
        "/api/users/123/posts/456/comments/789",
        "/api/users/alice/profile",
        "/assets/app/bundle",
        "/files/a/b/c/d.txt",
    };

    if (!runBench("BM_RouteMatchSmallParams", iterations, [&](size_t i) {
            const std::string& path = paths[i % paths.size()];
            auto match = router.findHandler(HttpMethod::GET, path);
            if (match.handler == nullptr) {
                return size_t{0};
            }
            return match.params.size() + match.params["id"].size();
        })) {
        return 1;
    }

    if (!runBench("BM_RouteMatchRequestParamInstall", iterations, [&](size_t i) {
            const std::string& path = paths[i % paths.size()];
            auto match = router.findHandler(HttpMethod::GET, path);
            if (match.handler == nullptr) {
                return size_t{0};
            }
            HttpRequest request;
            request.setRouteParams(std::move(match.params));
            return request.getRouteParam("id").size() +
                   request.getRouteParam("postId").size() +
                   request.getRouteParam("commentId").size() +
                   request.getRouteParam("missing", "fallback").size();
        })) {
        return 1;
    }

    return 0;
}
