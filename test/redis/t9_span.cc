#include <concepts>
#include <span>
#include <utility>

#include <galay/cpp/galay-redis/async/redis_client.h>

using namespace galay::redis;

namespace
{
    template <typename T>
    concept HasSpanBatchApi = requires(T& client, std::span<const RedisCommandView> commands) {
        { client.batch(commands) } -> std::same_as<RedisExchangeOperation>;
    };

    template <typename T>
    concept BuilderCommandsExposeSpan = requires(T& builder) {
        { builder.commands() } -> std::same_as<std::span<const RedisCommandView>>;
    };

    static_assert(HasSpanBatchApi<RedisClient>);
    static_assert(BuilderCommandsExposeSpan<RedisCommandBuilder>);
}

int main()
{
    return 0;
}
