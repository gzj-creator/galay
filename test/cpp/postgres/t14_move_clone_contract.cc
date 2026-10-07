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
consteval bool is_move_only_cloneable()
{
    return !std::is_copy_constructible_v<T> &&
           !std::is_copy_assignable_v<T> &&
           std::is_move_constructible_v<T> &&
           std::is_move_assignable_v<T> &&
           ExplicitlyCloneable<T>;
}

static_assert(is_move_only_cloneable<PostgresField>());
static_assert(is_move_only_cloneable<PostgresRow>());
static_assert(is_move_only_cloneable<PostgresResultSet>());
static_assert(is_move_only_cloneable<PostgresPrepareAwaitable<>::PrepareResult>());
static_assert(is_move_only_cloneable<PostgresClient::PrepareResult>());
static_assert(is_move_only_cloneable<PostgresEncodedBatch>());
static_assert(is_move_only_cloneable<PostgresCommandBuilder>());
static_assert(std::is_nothrow_move_constructible_v<PostgresCommandBuilder>);
static_assert(std::is_nothrow_move_assignable_v<PostgresCommandBuilder>);

int main()
{
    return 0;
}
