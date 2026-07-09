#include <concepts>
#include <iostream>
#include <type_traits>

#include <galay/cpp/galay-mongo/async/client.h>
#include <galay/cpp/galay-mongo/base/mongo_value.h>
#include <galay/cpp/galay-mongo/protoc/builder.h>
#include <galay/cpp/galay-mongo/protoc/mongo_protocol.h>

using namespace galay::mongo;
using namespace galay::mongo::protocol;

namespace
{

template <typename T>
concept HasValueClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template <typename T>
constexpr bool MoveOnlyCloneValue =
    !std::is_copy_constructible_v<T> &&
    !std::is_copy_assignable_v<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_move_assignable_v<T> &&
    HasValueClone<T>;

static_assert(MoveOnlyCloneValue<MongoValue>);
static_assert(MoveOnlyCloneValue<MongoArray>);
static_assert(MoveOnlyCloneValue<MongoDocument>);
static_assert(MoveOnlyCloneValue<MongoReply>);
static_assert(MoveOnlyCloneValue<MongoMessage>);
static_assert(MoveOnlyCloneValue<MongoCommandBuilder>);
static_assert(MoveOnlyCloneValue<MongoPipelineResponse>);

static_assert(std::is_copy_constructible_v<MongoError>);
static_assert(std::is_copy_assignable_v<MongoError>);

} // namespace

int main()
{
    std::cout << "=== T15: Mongo move-only ownership trait tests ===" << std::endl;
    std::cout << "All move-only ownership trait tests PASSED!" << std::endl;
    return 0;
}
