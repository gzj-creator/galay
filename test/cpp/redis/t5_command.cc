#include <chrono>
#include <concepts>
#include <utility>

#include <galay/cpp/galay-redis/async/redis_client.h>

using namespace galay::redis;

namespace
{
    template <typename T>
    concept HasRawCommandApi = requires(T& client, RedisEncodedCommand command) {
        { client.command(std::move(command)) } -> std::same_as<RedisExchangeOperation>;
        { client.command(std::move(command)).timeout(std::chrono::milliseconds(200)) };
    };

    static_assert(HasRawCommandApi<RedisClient>);
}

int main()
{
    return 0;
}
