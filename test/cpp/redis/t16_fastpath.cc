#include <chrono>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <galay/cpp/galay-redis/async/redis_client.h>

using namespace galay::redis;

namespace
{
    using DefaultRedisClient = RedisClient<>;

    template <typename T>
    concept HasBorrowedPlainFastPath = requires(T& client,
                                                RedisBorrowedCommand packet,
                                                const std::string& storage) {
        { client.commandBorrowed(packet) } -> std::same_as<RedisExchangeOperation>;
        { client.commandBorrowed(packet).timeout(std::chrono::milliseconds(1)) };
        { client.batchBorrowed(storage, size_t{2}) } -> std::same_as<RedisExchangeOperation>;
        { client.batchBorrowed(storage, size_t{2}).timeout(std::chrono::milliseconds(1)) };
    };

    template <typename T>
    concept RejectsTemporaryBatchString = !requires(T& client) {
        client.batchBorrowed(std::string("tmp"), size_t{1});
    };

    template <typename T>
    concept RejectsBatchStringView = !requires(T& client,
                                               std::string_view encoded) {
        client.batchBorrowed(encoded, size_t{1});
    };

    template <typename T>
    concept RejectsTemporaryBorrowedPacket = !requires(T& client,
                                                       RedisBorrowedCommand packet) {
        client.commandBorrowed(std::move(packet));
    };

    static_assert(HasBorrowedPlainFastPath<DefaultRedisClient>);
    static_assert(std::is_same_v<
                  decltype(static_cast<RedisExchangeOperation (DefaultRedisClient::*)(const RedisBorrowedCommand&)>(&DefaultRedisClient::commandBorrowed)),
                  RedisExchangeOperation (DefaultRedisClient::*)(const RedisBorrowedCommand&)>);
    static_assert(std::is_same_v<
                  decltype(static_cast<RedisExchangeOperation (DefaultRedisClient::*)(const std::string&, size_t)>(&DefaultRedisClient::batchBorrowed)),
                  RedisExchangeOperation (DefaultRedisClient::*)(const std::string&, size_t)>);
    static_assert(std::constructible_from<RedisBorrowedCommand, const std::string&, size_t>);
    static_assert(!std::constructible_from<RedisBorrowedCommand, std::string&&, size_t>);
    static_assert(!std::constructible_from<RedisBorrowedCommand, std::string_view, size_t>);
    static_assert(RejectsTemporaryBatchString<DefaultRedisClient>);
    static_assert(RejectsBatchStringView<DefaultRedisClient>);
    static_assert(RejectsTemporaryBorrowedPacket<DefaultRedisClient>);
}

int main()
{
    return 0;
}
