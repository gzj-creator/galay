#include <chrono>
#include <concepts>
#include <span>
#include <utility>

#include "galay-redis/async/redis_client.h"

using namespace galay::redis;

namespace
{
    template <typename T>
    concept HasBatchTimeoutApi = requires(T& client, std::span<const RedisCommandView> commands) {
        { client.batch(commands) } -> std::same_as<RedisExchangeOperation>;
        { client.batch(commands).timeout(std::chrono::milliseconds(200)) };
    };

    static_assert(HasBatchTimeoutApi<RedisClient>);
}

int main()
{
    return 0;
}
