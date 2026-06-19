#include "galay-redis/async/redis_client.h"
#include "galay-redis/async/conn_pool.h"
#include "galay-redis/async/topology_client.h"

#include <galay-kernel/core/runtime.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <type_traits>

using namespace galay::redis;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool success = false;
    std::string message;
};

void finish(TestState& state, bool success, std::string message)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.done = true;
    state.success = success;
    state.message = std::move(message);
    state.cv.notify_one();
}

std::optional<bool> parseBoolEnv(const char* value)
{
    if (value == nullptr) return std::nullopt;
    std::string text(value);
    if (text == "1" || text == "true" || text == "TRUE") return true;
    if (text == "0" || text == "false" || text == "FALSE") return false;
    return std::nullopt;
}

bool integrationEnabled()
{
    const char* value = std::getenv("GALAY_IT_ENABLE");
    if (value == nullptr) return false;
    std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" ||
           text == "YES" || text == "on" || text == "ON";
}

struct ParsedRedissUrl {
    std::string host;
    int32_t port = 6380;
};

std::optional<ParsedRedissUrl> parseRedissUrl(const std::string& url)
{
    static const std::regex kUrlPattern(
        R"(^(rediss)://(?:[^@/]+@)?(\[[^\]]+\]|[^:/?#]+)(?::(\d+))?(?:/\d+)?$)",
        std::regex::icase);

    std::smatch matches;
    if (!std::regex_match(url, matches, kUrlPattern)) {
        return std::nullopt;
    }

    ParsedRedissUrl parsed;
    parsed.host = matches[2].str();
    if (parsed.host.size() >= 2 && parsed.host.front() == '[' && parsed.host.back() == ']') {
        parsed.host = parsed.host.substr(1, parsed.host.size() - 2);
    }
    if (matches[3].matched) {
        try {
            parsed.port = static_cast<int32_t>(std::stoi(matches[3].str()));
        } catch (...) {
            return std::nullopt;
        }
    }
    return parsed;
}

#ifdef GALAY_SSL_FEATURE_ENABLED

static_assert(std::is_class_v<RedissConnectionPool>);
static_assert(std::is_class_v<RedissConnectionPoolConfig>);
static_assert(std::is_class_v<RedissMasterSlaveClient>);
static_assert(std::is_class_v<RedissMasterSlaveClientBuilder>);
static_assert(std::is_class_v<RedissClusterClient>);
static_assert(std::is_class_v<RedissClusterClientBuilder>);

static_assert(requires(RedissMasterSlaveClientBuilder builder, AsyncRedisConfig async_config, RedissClientConfig tls_config) {
    { builder.scheduler(static_cast<IOScheduler*>(nullptr)) } -> std::same_as<RedissMasterSlaveClientBuilder&>;
    { builder.config(async_config) } -> std::same_as<RedissMasterSlaveClientBuilder&>;
    { builder.tlsConfig(tls_config) } -> std::same_as<RedissMasterSlaveClientBuilder&>;
});

static_assert(requires(RedissClusterClientBuilder builder, AsyncRedisConfig async_config, RedissClientConfig tls_config) {
    { builder.scheduler(static_cast<IOScheduler*>(nullptr)) } -> std::same_as<RedissClusterClientBuilder&>;
    { builder.config(async_config) } -> std::same_as<RedissClusterClientBuilder&>;
    { builder.tlsConfig(tls_config) } -> std::same_as<RedissClusterClientBuilder&>;
});

Task<void> runRedissPoolAndTopologySmoke(IOScheduler* scheduler, TestState* state)
{
    const char* url = std::getenv("GALAY_REDIS_TLS_URL");
    if (url == nullptr || std::string(url).empty()) {
        finish(*state, true, "SKIP: GALAY_REDIS_TLS_URL not set");
        co_return;
    }

    auto parsed = parseRedissUrl(url);
    if (!parsed.has_value()) {
        finish(*state, false, "invalid GALAY_REDIS_TLS_URL");
        co_return;
    }

    RedissClientConfig tls_config;
    if (const char* ca_path = std::getenv("GALAY_REDIS_TLS_CA")) {
        tls_config.ca_path = ca_path;
    }
    if (const auto verify_peer = parseBoolEnv(std::getenv("GALAY_REDIS_TLS_VERIFY_PEER"))) {
        tls_config.verify_peer = *verify_peer;
    }
    if (const char* server_name = std::getenv("GALAY_REDIS_TLS_SERVER_NAME")) {
        tls_config.server_name = server_name;
    }

    RedisCommandBuilder builder;

    RedissConnectionPoolConfig pool_config = RedissConnectionPoolConfig::create(parsed->host, parsed->port, 1, 2);
    pool_config.initial_connections = 1;
    pool_config.tls_config = tls_config;

    RedissConnectionPool pool(scheduler, pool_config);
    auto init_result = co_await pool.initialize().timeout(5s);
    if (!init_result) {
        finish(*state, false, "pool init failed: " + init_result.error().message());
        co_return;
    }

    auto acquire_result = co_await pool.acquire().timeout(5s);
    if (!acquire_result) {
        finish(*state, false, "pool acquire failed: " + acquire_result.error().message());
        co_return;
    }

    auto conn = acquire_result.value();
    auto connect_result = co_await conn->get()->connect(url).timeout(5s);
    if (!connect_result) {
        finish(*state, false, "pool client connect failed: " + connect_result.error().message());
        co_return;
    }

    auto ping_result = co_await conn->get()->command(builder.ping()).timeout(5s);
    if (!ping_result || !ping_result.value().has_value()) {
        finish(*state, false, "pool client ping failed");
        co_return;
    }
    pool.release(conn);
    pool.shutdown();

    if (const char* sentinel_host = std::getenv("GALAY_REDIS_TLS_SENTINEL_HOST")) {
        const char* sentinel_port = std::getenv("GALAY_REDIS_TLS_SENTINEL_PORT");
        RedissMasterSlaveClient ms = RedissMasterSlaveClientBuilder()
                                         .scheduler(scheduler)
                                         .tlsConfig(tls_config)
                                         .build();
        if (const char* master_name = std::getenv("GALAY_REDIS_TLS_SENTINEL_MASTER_NAME")) {
            ms.setSentinelMasterName(master_name);
        }

        RedisNodeAddress sentinel;
        sentinel.host = sentinel_host;
        sentinel.port = sentinel_port ? static_cast<int32_t>(std::stoi(sentinel_port)) : 26379;

        auto sentinel_connect = co_await ms.addSentinel(sentinel).timeout(5s);
        if (!sentinel_connect) {
            finish(*state, false, "tls sentinel connect failed: " + sentinel_connect.error().message());
            co_return;
        }

        auto refresh_result = co_await ms.refreshFromSentinel();
        if (!refresh_result) {
            finish(*state, false, std::string("tls sentinel refresh failed: ") +
                                      std::string(refresh_result.error().message()));
            co_return;
        }
    }

    if (const char* cluster_host = std::getenv("GALAY_REDIS_TLS_CLUSTER_HOST")) {
        const char* cluster_port = std::getenv("GALAY_REDIS_TLS_CLUSTER_PORT");

        RedissClusterClient cluster = RedissClusterClientBuilder()
                                          .scheduler(scheduler)
                                          .tlsConfig(tls_config)
                                          .build();

        RedisClusterNodeAddress node;
        node.host = cluster_host;
        node.port = cluster_port ? static_cast<int32_t>(std::stoi(cluster_port)) : parsed->port;
        auto node_connect = co_await cluster.addNode(node).timeout(5s);
        if (!node_connect) {
            finish(*state, false, "tls cluster addNode failed: " + node_connect.error().message());
            co_return;
        }

        auto refresh_result = co_await cluster.refreshSlots();
        if (!refresh_result) {
            finish(*state, false, std::string("tls cluster refresh failed: ") +
                                      std::string(refresh_result.error().message()));
            co_return;
        }
    }

    finish(*state, true, "PASS");
}

#endif

} // namespace

int main()
{
#ifndef GALAY_SSL_FEATURE_ENABLED
    std::cout << "SKIP: GALAY_SSL_FEATURE_ENABLED not set\n";
    return 0;
#else
    if (!integrationEnabled()) {
        std::cout << "[SKIP] set GALAY_IT_ENABLE=1 to run TLS topology integration test\n";
        return 0;
    }

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler\n";
        runtime.stop();
        return 1;
    }

    TestState state;
    scheduleTask(scheduler, runRedissPoolAndTopologySmoke(scheduler, &state));

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool done = state.cv.wait_for(lock, 15s, [&state]() { return state.done; });
    runtime.stop();

    if (!done) {
        std::cerr << "T19 timed out\n";
        return 1;
    }

    std::cout << state.message << "\n";
    return state.success ? 0 : 1;
#endif
}
