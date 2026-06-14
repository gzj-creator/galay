#include "redis/async/redis_client.h"
#include "redis/async/topology_client.h"
#include <kernel/kernel/task.h>
#include <chrono>
#include <concepts>
#include <iostream>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

using namespace galay::redis;
using galay::kernel::Task;

static_assert(std::is_class_v<RedissClient>);
static_assert(std::is_class_v<RedissClientBuilder>);
static_assert(std::is_class_v<RedissClientConfig>);

static_assert(requires(RedissClientBuilder builder, AsyncRedisConfig async_config, RedissClientConfig tls_config) {
    { builder.config(async_config) } -> std::same_as<RedissClientBuilder&>;
    { builder.tlsConfig(tls_config) } -> std::same_as<RedissClientBuilder&>;
    { builder.caPath(std::string()) } -> std::same_as<RedissClientBuilder&>;
    { builder.verifyPeer(true) } -> std::same_as<RedissClientBuilder&>;
    { builder.verifyDepth(4) } -> std::same_as<RedissClientBuilder&>;
    { builder.serverName(std::string()) } -> std::same_as<RedissClientBuilder&>;
});

static_assert(requires(RedissClient& client, std::string url) {
    { client.connect(url) };
    { client.connect(std::string("rediss://127.0.0.1:6380")) };
});

static_assert(requires(RedissClient& client,
                       RedisEncodedCommand encoded,
                       std::span<const RedisCommandView> commands,
                       RedisConnectOptions options) {
    { client.command(std::move(encoded)).timeout(std::chrono::milliseconds(1)) };
    { client.receive(1).timeout(std::chrono::milliseconds(1)) };
    { client.batch(commands).timeout(std::chrono::milliseconds(1)) };
    { client.connect(std::string("127.0.0.1"), 6380, std::move(options)).timeout(std::chrono::milliseconds(1)) };
});

static_assert(std::same_as<
    decltype(std::declval<RedisMasterSlaveClient&>().execute(
        std::declval<const std::string&>(),
        std::declval<const std::vector<std::string>&>(),
        false,
        true)),
    Task<RedisCommandResult>>);

static_assert(std::same_as<
    decltype(std::declval<RedisMasterSlaveClient&>().refreshFromSentinel()),
    Task<RedisCommandResult>>);

static_assert(std::same_as<
    decltype(std::declval<RedisClusterClient&>().execute(
        std::declval<const std::string&>(),
        std::declval<const std::vector<std::string>&>(),
        std::declval<std::string>(),
        true)),
    Task<RedisCommandResult>>);

static_assert(std::same_as<
    decltype(std::declval<RedisClusterClient&>().refreshSlots()),
    Task<RedisCommandResult>>);

int main()
{
    std::cout << "T16-RedissSurface PASS\n";
    return 0;
}
