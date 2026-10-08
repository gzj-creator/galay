#include <cassert>
#include <chrono>
#include <coroutine>
#include <type_traits>
#include <utility>

import galay.utils;
import galay.kernel;
import galay.ssl;
import galay.http;
import galay.websocket;
import galay.http2;
import galay.redis;
import galay.etcd;
import galay.mysql;
import galay.postgres;
import galay.mongo;
import galay.rpc;
import galay.rpc.etcd;
import galay.mcp;
import galay.tracing;
import json;
import toml;

template<class Builder>
concept HasDocument = requires(const Builder& builder) { builder.export_openapi(); };

galay::kernel::Task<galay::http::HttpResponseResult> native_response(galay::http::HttpRequest)
{
    co_return galay::http::HttpResponse{};
}

int main()
{
    const auto page_size = galay::utils::Memory::page_size();
    assert(page_size && *page_size > 0);
    galay::http::HttpRequest request;
    auto config = galay::redis::ConnectionPoolConfig::create("127.0.0.1", 1, 0, 1);
    galay::redis::RedisConnectionPool pool(nullptr, config);
    auto initialized = pool.initialize().timeout(std::chrono::milliseconds(1));
    assert(initialized.await_ready());
    assert(initialized.await_resume().has_value());
    pool.shutdown();

    static_assert(std::is_move_constructible_v<galay::redis::RedissPoolInitializeAwaitable>);
    static_assert(!std::is_move_constructible_v<galay::kernel::IOController>);
    static_assert(sizeof(galay::postgres::PostgresConfig) > 0);
    static_assert(sizeof(galay::mongo::MongoConfig) > 0);
    static_assert(sizeof(galay::mongo::protocol::MongoCommandBuilder) > 0);
    static_assert(sizeof(galay::tracing::TraceId) > 0);

    static_assert(!HasDocument<galay::http::HttpServerBuilder<>>);
    static_assert(HasDocument<galay::http::HttpServerBuilder<true>>);
    static_assert(!HasDocument<galay::http2::H2cServerBuilder<>>);
    static_assert(HasDocument<galay::http2::H2cServerBuilder<true>>);
    galay::http::HttpServerBuilder<> http;
    const auto registered = http.add_request_handler<galay::http::HttpMethod::GET>("/native", native_response);
    assert(registered.has_value());
    const auto duplicate = http.add_request_handler<galay::http::HttpMethod::GET>("/native", native_response);
    assert(!duplicate && duplicate.error().code == galay::api::ApiErrorCode::kRouteConflict);
    galay::http2::H2cServerBuilder<> h2c;
    const auto h2_registered = h2c.add_request_handler<galay::http::HttpMethod::GET>("/native", native_response);
    assert(h2_registered.has_value());
}
