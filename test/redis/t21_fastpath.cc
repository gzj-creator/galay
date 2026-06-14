#include <chrono>
#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "redis/async/redis_client.h"

using namespace galay::redis;

namespace
{
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

    static_assert(HasBorrowedPlainFastPath<RedisClient>);
    static_assert(std::is_same_v<
                  decltype(static_cast<RedisExchangeOperation (RedisClient::*)(const RedisBorrowedCommand&)>(&RedisClient::commandBorrowed)),
                  RedisExchangeOperation (RedisClient::*)(const RedisBorrowedCommand&)>);
    static_assert(std::is_same_v<
                  decltype(static_cast<RedisExchangeOperation (RedisClient::*)(const std::string&, size_t)>(&RedisClient::batchBorrowed)),
                  RedisExchangeOperation (RedisClient::*)(const std::string&, size_t)>);
    static_assert(std::constructible_from<RedisBorrowedCommand, const std::string&, size_t>);
    static_assert(!std::constructible_from<RedisBorrowedCommand, std::string&&, size_t>);
    static_assert(!std::constructible_from<RedisBorrowedCommand, std::string_view, size_t>);
    static_assert(RejectsTemporaryBatchString<RedisClient>);
    static_assert(RejectsBatchStringView<RedisClient>);
    static_assert(RejectsTemporaryBorrowedPacket<RedisClient>);
}

int main()
{
    return 0;
}
