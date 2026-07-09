#include <galay/cpp/galay-redis/async/conn_pool.h>
#include <galay/cpp/galay-redis/async/topology_client.h>
#include <galay/cpp/galay-redis/base/redis_config.h>
#include <galay/cpp/galay-redis/base/redis_error.h>
#include <galay/cpp/galay-redis/base/redis_value.h>
#include <galay/cpp/galay-redis/protoc/builder.h>
#include <galay/cpp/galay-redis/protoc/connection.h>
#include <galay/cpp/galay-redis/protoc/redis_protocol.h>
#include <galay/cpp/galay-redis/sync/redis_session.h>
#ifdef GALAY_SSL_FEATURE_ENABLED
#include <galay/cpp/galay-ssl/common/error.h>
#endif

#include <array>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::redis;
using namespace galay::redis::protocol;

namespace
{

template <typename T>
concept ExplicitClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template <typename T>
constexpr bool move_only_v =
    !std::is_copy_constructible_v<T> &&
    !std::is_copy_assignable_v<T> &&
    std::is_move_constructible_v<T> &&
    std::is_move_assignable_v<T>;

static_assert(move_only_v<Connection>);
static_assert(move_only_v<RedisReply>);
static_assert(ExplicitClone<RedisReply>);
static_assert(move_only_v<RedisValue>);
static_assert(ExplicitClone<RedisValue>);
static_assert(move_only_v<RedisAsyncValue>);
static_assert(ExplicitClone<RedisAsyncValue>);
static_assert(move_only_v<RedisEncodedCommand>);
static_assert(ExplicitClone<RedisEncodedCommand>);
static_assert(move_only_v<RedisCommandBuilder>);
static_assert(ExplicitClone<RedisCommandBuilder>);
static_assert(move_only_v<PooledConnection>);
static_assert(move_only_v<RedisSession>);

#ifdef GALAY_SSL_FEATURE_ENABLED
static_assert(move_only_v<PooledRedissConnection>);
#endif

static_assert(std::is_copy_constructible_v<RedisSessionConfig>);
static_assert(std::is_copy_assignable_v<RedisSessionConfig>);
static_assert(std::is_copy_constructible_v<AsyncRedisConfig>);
static_assert(std::is_copy_assignable_v<AsyncRedisConfig>);
static_assert(std::is_copy_constructible_v<ConnectionPoolConfig>);
static_assert(std::is_copy_assignable_v<ConnectionPoolConfig>);
static_assert(std::is_copy_constructible_v<RedisConnectOptions>);
static_assert(std::is_copy_assignable_v<RedisConnectOptions>);
static_assert(std::is_copy_constructible_v<RedissClientConfig>);
static_assert(std::is_copy_assignable_v<RedissClientConfig>);
static_assert(std::is_copy_constructible_v<RedisTopologyRetryConfig>);
static_assert(std::is_copy_assignable_v<RedisTopologyRetryConfig>);
static_assert(std::is_copy_constructible_v<RedisTopologyRefreshConfig>);
static_assert(std::is_copy_assignable_v<RedisTopologyRefreshConfig>);
static_assert(std::is_copy_constructible_v<RedisTopologyStats>);
static_assert(std::is_copy_assignable_v<RedisTopologyStats>);
static_assert(std::is_copy_constructible_v<RedisNodeAddress>);
static_assert(std::is_copy_assignable_v<RedisNodeAddress>);
static_assert(std::is_copy_constructible_v<RedisClusterNodeAddress>);
static_assert(std::is_copy_assignable_v<RedisClusterNodeAddress>);
static_assert(std::is_copy_constructible_v<RedisCommandView>);
static_assert(std::is_copy_assignable_v<RedisCommandView>);
static_assert(std::is_copy_constructible_v<RedisBorrowedCommand>);
static_assert(std::is_copy_assignable_v<RedisBorrowedCommand>);
static_assert(std::is_copy_constructible_v<RespParser>);
static_assert(std::is_copy_assignable_v<RespParser>);
static_assert(std::is_copy_constructible_v<RespEncoder>);
static_assert(std::is_copy_assignable_v<RespEncoder>);
static_assert(std::is_copy_constructible_v<RedisError>);
static_assert(std::is_copy_assignable_v<RedisError>);
#ifdef GALAY_SSL_FEATURE_ENABLED
static_assert(std::is_constructible_v<RedisError, const galay::ssl::SslError&>);
#endif
static_assert(!std::is_copy_constructible_v<RedisConnectionPool>);
static_assert(!std::is_move_constructible_v<RedisConnectionPool>);

#ifdef GALAY_SSL_FEATURE_ENABLED
static_assert(std::is_copy_constructible_v<RedissConnectionPoolConfig>);
static_assert(std::is_copy_assignable_v<RedissConnectionPoolConfig>);
static_assert(!std::is_copy_constructible_v<RedissConnectionPool>);
static_assert(!std::is_move_constructible_v<RedissConnectionPool>);
#endif

RedisReply makeNestedReply()
{
    std::vector<RedisReply> inner;
    inner.emplace_back(RespType::BulkString, std::string("leaf"));

    std::vector<std::pair<RedisReply, RedisReply>> map_entries;
    map_entries.emplace_back(RedisReply(RespType::SimpleString, std::string("key")),
                             RedisReply(RespType::Array, std::move(inner)));

    std::vector<RedisReply> outer;
    outer.emplace_back(RespType::Map, std::move(map_entries));
    outer.emplace_back(RespType::Integer, int64_t{42});

    return RedisReply(RespType::Array, std::move(outer));
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        return false;
    }
    return true;
}

bool testRedisReplyClone()
{
    RedisReply original = makeNestedReply();
    RedisReply cloned = original.clone();
    RedisReply moved_original = std::move(original);

    const auto& cloned_array = cloned.asArray();
    if (!expect(cloned_array.size() == 2, "cloned reply array size mismatch")) {
        return false;
    }
    if (!expect(cloned_array[1].asInteger() == 42, "cloned reply integer mismatch")) {
        return false;
    }

    const auto& cloned_map = cloned_array[0].asMap();
    if (!expect(cloned_map.size() == 1, "cloned reply map size mismatch")) {
        return false;
    }
    if (!expect(cloned_map[0].first.asString() == "key", "cloned reply map key mismatch")) {
        return false;
    }
    if (!expect(cloned_map[0].second.asArray()[0].asString() == "leaf",
                "cloned reply nested payload mismatch")) {
        return false;
    }
    return expect(moved_original.asArray().size() == 2, "moved reply lost payload");
}

bool testRedisValueCloneAndConversions()
{
    RedisValue value(makeNestedReply());
    RedisValue cloned = value.clone();
    RedisValue moved = std::move(cloned);

    const std::vector<RedisValue> array = moved.toArray();
    if (!expect(array.size() == 2, "RedisValue clone array size mismatch")) {
        return false;
    }
    if (!expect(array[1].toInteger() == 42, "RedisValue clone integer mismatch")) {
        return false;
    }

    const std::map<std::string, RedisValue> map = array[0].toMap();
    auto it = map.find("key");
    if (!expect(it != map.end(), "RedisValue clone map key missing")) {
        return false;
    }
    const std::vector<RedisValue> nested = it->second.toArray();
    if (!expect(nested.size() == 1, "RedisValue clone nested array size mismatch")) {
        return false;
    }
    if (!expect(nested[0].toString() == "leaf", "RedisValue clone nested payload mismatch")) {
        return false;
    }

    std::vector<RedisReply> set_items;
    set_items.emplace_back(RespType::BulkString, std::string("member"));
    RedisValue set_value(RedisReply(RespType::Set, std::move(set_items)));
    if (!expect(set_value.toSet()[0].toString() == "member", "RedisValue set conversion mismatch")) {
        return false;
    }

    std::vector<RedisReply> push_items;
    push_items.emplace_back(RespType::SimpleString, std::string("message"));
    RedisValue push_value(RedisReply(RespType::Push, std::move(push_items)));
    if (!expect(push_value.toPush()[0].toStatus() == "message", "RedisValue push conversion mismatch")) {
        return false;
    }

    RedisAsyncValue async_value(makeNestedReply());
    RedisAsyncValue async_clone = async_value.clone();
    return expect(async_clone.toArray().size() == 2, "RedisAsyncValue clone array size mismatch");
}

bool testRedisEncodedCommandClone()
{
    RedisEncodedCommand command;
    command.encoded = "*1\r\n$4\r\nPING\r\n";
    command.expected_replies = 3;

    RedisEncodedCommand cloned = command.clone();
    command.encoded = "changed";
    command.expected_replies = 1;

    if (!expect(cloned.encoded == "*1\r\n$4\r\nPING\r\n", "encoded command clone payload mismatch")) {
        return false;
    }
    if (!expect(cloned.expected_replies == 3, "encoded command clone reply count mismatch")) {
        return false;
    }

    RedisEncodedCommand moved = std::move(cloned);
    return expect(moved.expected_replies == 3, "encoded command move reply count mismatch");
}

bool testRedisCommandBuilderCloneAndMoveRebuildViews()
{
    RedisCommandBuilder builder;
    const std::array<std::string_view, 2> args{"alpha-key", "alpha-value"};
    builder.append("SET", args);
    const auto cached_before_clone = builder.commands();
    if (!expect(cached_before_clone.size() == 1, "builder cached command count mismatch")) {
        return false;
    }

    RedisCommandBuilder cloned = builder.clone();
    builder.clear();

    const auto cloned_views = cloned.commands();
    if (!expect(cloned_views.size() == 1, "builder clone command count mismatch")) {
        return false;
    }
    if (!expect(cloned_views[0].command == "SET", "builder clone command mismatch")) {
        return false;
    }
    if (!expect(cloned_views[0].args.size() == 2, "builder clone arg count mismatch")) {
        return false;
    }
    if (!expect(cloned_views[0].args[0] == "alpha-key", "builder clone first arg mismatch")) {
        return false;
    }
    if (!expect(cloned_views[0].encoded == cloned.encoded(), "builder clone encoded view mismatch")) {
        return false;
    }

    RedisCommandBuilder moved = std::move(cloned);
    const auto moved_views = moved.commands();
    if (!expect(moved_views.size() == 1, "builder move command count mismatch")) {
        return false;
    }
    if (!expect(moved_views[0].args[1] == "alpha-value", "builder move second arg mismatch")) {
        return false;
    }
    return expect(moved_views[0].encoded == moved.encoded(), "builder move encoded view mismatch");
}

} // namespace

int main()
{
    if (!testRedisReplyClone()) {
        return 1;
    }
    if (!testRedisValueCloneAndConversions()) {
        return 1;
    }
    if (!testRedisEncodedCommandClone()) {
        return 1;
    }
    if (!testRedisCommandBuilderCloneAndMoveRebuildViews()) {
        return 1;
    }

    std::cout << "T27-MoveCloneContract PASS\n";
    return 0;
}
