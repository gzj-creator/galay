#include <galay/cpp/galay-postgres/async/client.h>
#include <galay/cpp/galay-postgres/protoc/builder.h>

#include <array>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace galay::postgres;
using galay::utils::RingBuffer;
using galay::utils::RingBufferBackendStrategy;

namespace
{

using DefaultClient = AsyncPostgresClient<>;
using VectorClient = AsyncPostgresClient<RingBufferBackendStrategy::Vector>;
using DefaultConnect = PostgresConnectAwaitable<>;
using DefaultQuery = PostgresQueryAwaitable<>;
using DefaultPrepare = PostgresPrepareAwaitable<>;
using DefaultExecute = PostgresExecuteAwaitable<>;
using DefaultPipeline = PostgresPipelineAwaitable<>;

static_assert(!std::is_copy_constructible_v<DefaultClient>);
static_assert(!std::is_copy_assignable_v<DefaultClient>);
static_assert(std::is_nothrow_move_constructible_v<DefaultClient>);
static_assert(std::is_nothrow_move_assignable_v<DefaultClient>);

static_assert(!std::is_copy_constructible_v<DefaultConnect>);
static_assert(!std::is_copy_constructible_v<DefaultQuery>);
static_assert(!std::is_copy_constructible_v<DefaultPrepare>);
static_assert(!std::is_copy_constructible_v<DefaultExecute>);
static_assert(!std::is_copy_constructible_v<DefaultPipeline>);
static_assert(!std::is_copy_assignable_v<DefaultConnect>);
static_assert(!std::is_copy_assignable_v<DefaultQuery>);
static_assert(!std::is_copy_assignable_v<DefaultPrepare>);
static_assert(!std::is_copy_assignable_v<DefaultExecute>);
static_assert(!std::is_copy_assignable_v<DefaultPipeline>);
static_assert(std::is_nothrow_move_constructible_v<DefaultConnect>);
static_assert(std::is_nothrow_move_constructible_v<DefaultQuery>);
static_assert(std::is_nothrow_move_constructible_v<DefaultPrepare>);
static_assert(std::is_nothrow_move_constructible_v<DefaultExecute>);
static_assert(std::is_nothrow_move_constructible_v<DefaultPipeline>);
static_assert(std::is_nothrow_move_assignable_v<DefaultConnect>);
static_assert(std::is_nothrow_move_assignable_v<DefaultQuery>);
static_assert(std::is_nothrow_move_assignable_v<DefaultPrepare>);
static_assert(std::is_nothrow_move_assignable_v<DefaultExecute>);
static_assert(std::is_nothrow_move_assignable_v<DefaultPipeline>);

static_assert(requires(DefaultClient& client,
                       PostgresConfig config,
                       std::span<const std::optional<std::string>> owned_params,
                       std::span<const std::optional<std::string_view>> viewed_params,
                       std::span<const std::string_view> sqls,
                       std::span<const protocol::PostgresCommandView> commands) {
    { client.connect(std::move(config)) } -> std::same_as<DefaultConnect>;
    { client.query("SELECT 1") } -> std::same_as<DefaultQuery>;
    { client.prepare("galay_stmt", "SELECT $1") } -> std::same_as<DefaultPrepare>;
    { client.execute("galay_stmt", owned_params) } -> std::same_as<DefaultExecute>;
    { client.execute("galay_stmt", viewed_params) } -> std::same_as<DefaultExecute>;
    { client.pipeline(sqls) } -> std::same_as<DefaultPipeline>;
    { client.batch(commands) } -> std::same_as<DefaultPipeline>;
    { client.begin_transaction() } -> std::same_as<DefaultQuery>;
    { client.commit() } -> std::same_as<DefaultQuery>;
    { client.rollback() } -> std::same_as<DefaultQuery>;
    { client.transaction_status() } -> std::same_as<char>;
});

static_assert(requires(VectorClient& client) {
    { client.query("SELECT 1") } -> std::same_as<PostgresQueryAwaitable<RingBufferBackendStrategy::Vector>>;
    { client.ring_buffer() } ->
        std::same_as<RingBuffer<RingBufferBackendStrategy::Vector, std::dynamic_extent>&>;
});

} // namespace

int main()
{
    AsyncPostgresClient<> client(nullptr);
    auto invalid_query = client.query({});
    if (!invalid_query.is_invalid()) {
        return 1;
    }

    auto invalid_host = client.connect("", 5432, "user", "password");
    if (!invalid_host.is_invalid()) {
        return 2;
    }
    auto invalid_port = client.connect("127.0.0.1", 0, "user", "password");
    if (!invalid_port.is_invalid()) {
        return 3;
    }
    auto invalid_user = client.connect("127.0.0.1", 5432, "", "password");
    if (!invalid_user.is_invalid()) {
        return 4;
    }
    auto invalid_address = client.connect("not-an-ip-address", 5432, "user", "password");
    if (!invalid_address.is_invalid()) {
        return 5;
    }

    std::array<protocol::PostgresCommandView, 1> invalid_commands{
        protocol::PostgresCommandView{"", protocol::PostgresCommandKind::Query}};
    auto invalid_batch = client.batch(invalid_commands);
    if (!invalid_batch.is_invalid()) {
        return 6;
    }

    protocol::PostgresCommandBuilder builder;
    builder.append_parse("stmt", "SELECT 1");
    auto missing_boundary = client.batch(builder.commands());
    if (!missing_boundary.is_invalid()) {
        return 7;
    }

    auto empty_pipeline = client.pipeline({});
    return empty_pipeline.is_invalid() ? 8 : 0;
}
