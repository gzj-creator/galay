#include <cassert>
#include <chrono>
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

int main()
{
    const auto page_size = galay::utils::Memory::pageSize();
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
}
