#include <galay/cpp/galay-postgres/async/client.h>
#include <galay/cpp/galay-postgres/base/postgres_value.h>
#include <galay/cpp/galay-postgres/protoc/builder.h>
#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <concepts>
#include <type_traits>

using namespace galay::postgres;
using namespace galay::postgres::protocol;

template<typename T>
concept ExplicitlyCloneable = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template<typename T>
consteval bool isMoveOnlyCloneable()
{
    return !std::is_copy_constructible_v<T> &&
           !std::is_copy_assignable_v<T> &&
           std::is_move_constructible_v<T> &&
           std::is_move_assignable_v<T> &&
           ExplicitlyCloneable<T>;
}

static_assert(isMoveOnlyCloneable<PostgresField>());
static_assert(isMoveOnlyCloneable<PostgresRow>());
static_assert(isMoveOnlyCloneable<PostgresResultSet>());
static_assert(isMoveOnlyCloneable<PostgresPrepareAwaitable<>::PrepareResult>());
static_assert(isMoveOnlyCloneable<PostgresClient::PrepareResult>());
static_assert(isMoveOnlyCloneable<PostgresEncodedBatch>());
static_assert(isMoveOnlyCloneable<PostgresCommandBuilder>());
static_assert(std::is_nothrow_move_constructible_v<PostgresCommandBuilder>);
static_assert(std::is_nothrow_move_assignable_v<PostgresCommandBuilder>);

int main()
{
    return 0;
}
