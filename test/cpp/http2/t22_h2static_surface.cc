/**
 * @file t87_h2static_surface.cc
 * @brief HTTP/2 静态响应配置入口 public surface 测试
 */

#include <galay/cpp/galay-http2/client/h2_client.h>
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>

#include <cassert>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

using namespace galay::http2;
using galay::utils::RingBufferBackendStrategy;

static_assert(std::is_move_constructible_v<H2cClient<>>);
static_assert(std::is_move_constructible_v<H2cClient<RingBufferBackendStrategy::Vector>>);
static_assert(std::is_same_v<decltype(std::declval<H2cClientBuilder>().build()), H2cClient<>>);

#ifdef GALAY_SSL_FEATURE_ENABLED
static_assert(std::is_move_constructible_v<H2Client<>>);
static_assert(std::is_move_constructible_v<H2Client<RingBufferBackendStrategy::Vector>>);
static_assert(std::is_same_v<decltype(std::declval<H2ClientBuilder>().build()), H2Client<>>);
#endif

int main()
{
    H2cServerBuilder builder;
    builder.staticResponse("/echo", H2StaticResponse{
        .status = 200,
        .content_type = "text/plain",
        .body = "",
    });

    auto config = builder.buildConfig();
    assert(config.static_routes.size() == 1);
    assert(config.static_routes[0].path == "/echo");
    assert(config.static_routes[0].response.status == 200);
    assert(config.static_routes[0].response.content_type == "text/plain");
    assert(config.static_routes[0].response.body.empty());
    assert(config.static_routes[0].response.allow_head);

#ifdef GALAY_SSL_FEATURE_ENABLED
    H2ServerBuilder tls_builder;
    tls_builder.staticResponse("/tls", H2StaticResponse{
        .status = 204,
        .content_type = "text/plain",
        .body = "",
        .allow_head = false,
    });
    auto tls_config = tls_builder.buildConfig();
    assert(tls_config.static_routes.size() == 1);
    assert(tls_config.static_routes[0].path == "/tls");
    assert(tls_config.static_routes[0].response.status == 204);
    assert(!tls_config.static_routes[0].response.allow_head);
#endif

    std::cout << "t87_h2static_surface PASS\n";
    return 0;
}
