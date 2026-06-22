#include <galay/cpp/galay-mysql/async/client.h>
#include <galay/cpp/galay-mysql/sync/mysql_client.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <cassert>
#include <array>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

using namespace galay::mysql;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

template <typename T>
concept HasProtocolConnectAwaitable = requires { typename T::ProtocolConnectAwaitable; };

template <typename T>
concept HasProtocolHandshakeRecvAwaitable = requires { typename T::ProtocolHandshakeRecvAwaitable; };

template <typename T>
concept HasProtocolAuthSendAwaitable = requires { typename T::ProtocolAuthSendAwaitable; };

template <typename T>
concept HasProtocolAuthResultRecvAwaitable = requires { typename T::ProtocolAuthResultRecvAwaitable; };

template <typename T>
concept HasProtocolSendAwaitable = requires { typename T::ProtocolSendAwaitable; };

template <typename T>
concept HasProtocolRecvAwaitable = requires { typename T::ProtocolRecvAwaitable; };

static_assert(!std::derived_from<MysqlConnectAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolConnectAwaitable<MysqlConnectAwaitable>);
static_assert(!HasProtocolHandshakeRecvAwaitable<MysqlConnectAwaitable>);
static_assert(!HasProtocolAuthSendAwaitable<MysqlConnectAwaitable>);
static_assert(!HasProtocolAuthResultRecvAwaitable<MysqlConnectAwaitable>);

static_assert(!std::derived_from<MysqlQueryAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<MysqlQueryAwaitable>);
static_assert(!HasProtocolRecvAwaitable<MysqlQueryAwaitable>);

static_assert(!std::derived_from<MysqlPrepareAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<MysqlPrepareAwaitable>);
static_assert(!HasProtocolRecvAwaitable<MysqlPrepareAwaitable>);

static_assert(!std::derived_from<MysqlStmtExecuteAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<MysqlStmtExecuteAwaitable>);
static_assert(!HasProtocolRecvAwaitable<MysqlStmtExecuteAwaitable>);

static_assert(!std::derived_from<MysqlPipelineAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<MysqlPipelineAwaitable>);
static_assert(!HasProtocolRecvAwaitable<MysqlPipelineAwaitable>);

static_assert(requires(AsyncMysqlClient& client, MysqlConfig config) {
    { client.connect(config) } -> std::same_as<MysqlConnectAwaitable>;
    { client.query("SELECT 1") } -> std::same_as<MysqlQueryAwaitable>;
    { client.prepare("SELECT ?") } -> std::same_as<MysqlPrepareAwaitable>;
    { client.stmtExecute(1u,
                         std::declval<std::span<const std::optional<std::string>>>(),
                         std::declval<std::span<const uint8_t>>()) } -> std::same_as<MysqlStmtExecuteAwaitable>;
    { client.batch(std::declval<std::span<const protocol::MysqlCommandView>>()) } -> std::same_as<MysqlPipelineAwaitable>;
});

int main()
{
    const std::string oversized_sql(protocol::MYSQL_MAX_PACKET_SIZE, 'x');

    MysqlClient sync_client;
    auto sync_result = sync_client.query(oversized_sql);
    require(!sync_result.has_value(), "oversized sync query should fail before network IO");
    require(sync_result.error().type() == MYSQL_ERROR_INVALID_PARAM,
            "oversized sync query should return invalid param");
    std::array<std::string_view, 1> oversized_pipeline{std::string_view(oversized_sql)};
    auto sync_pipeline = sync_client.pipeline(oversized_pipeline);
    require(!sync_pipeline.has_value(), "oversized sync pipeline should fail before network IO");
    require(sync_pipeline.error().type() == MYSQL_ERROR_INVALID_PARAM,
            "oversized sync pipeline should return invalid param");

    AsyncMysqlClient async_client(nullptr);
    auto async_query = async_client.query(oversized_sql);
    require(async_query.isInvalid(), "oversized async query awaitable should be invalid");
    auto async_pipeline = async_client.pipeline(oversized_pipeline);
    require(async_pipeline.isInvalid(), "oversized async pipeline awaitable should be invalid");

    return 0;
}
