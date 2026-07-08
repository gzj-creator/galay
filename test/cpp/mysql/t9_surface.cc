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
using galay::utils::RingBufferBackendStrategy;
using galay::utils::RingBuffer;

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

using DefaultMysqlClient = AsyncMysqlClient<>;
using VectorMysqlClient = AsyncMysqlClient<RingBufferBackendStrategy::Vector>;
using DefaultMysqlConnectAwaitable = MysqlConnectAwaitable<>;
using DefaultMysqlQueryAwaitable = MysqlQueryAwaitable<>;
using DefaultMysqlPrepareAwaitable = MysqlPrepareAwaitable<>;
using DefaultMysqlStmtExecuteAwaitable = MysqlStmtExecuteAwaitable<>;
using DefaultMysqlPipelineAwaitable = MysqlPipelineAwaitable<>;
using VectorMysqlConnectAwaitable = MysqlConnectAwaitable<RingBufferBackendStrategy::Vector>;
using VectorMysqlQueryAwaitable = MysqlQueryAwaitable<RingBufferBackendStrategy::Vector>;
using VectorMysqlPrepareAwaitable = MysqlPrepareAwaitable<RingBufferBackendStrategy::Vector>;
using VectorMysqlStmtExecuteAwaitable = MysqlStmtExecuteAwaitable<RingBufferBackendStrategy::Vector>;
using VectorMysqlPipelineAwaitable = MysqlPipelineAwaitable<RingBufferBackendStrategy::Vector>;

static_assert(!std::derived_from<DefaultMysqlConnectAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolConnectAwaitable<DefaultMysqlConnectAwaitable>);
static_assert(!HasProtocolHandshakeRecvAwaitable<DefaultMysqlConnectAwaitable>);
static_assert(!HasProtocolAuthSendAwaitable<DefaultMysqlConnectAwaitable>);
static_assert(!HasProtocolAuthResultRecvAwaitable<DefaultMysqlConnectAwaitable>);

static_assert(!std::derived_from<DefaultMysqlQueryAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<DefaultMysqlQueryAwaitable>);
static_assert(!HasProtocolRecvAwaitable<DefaultMysqlQueryAwaitable>);

static_assert(!std::derived_from<DefaultMysqlPrepareAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<DefaultMysqlPrepareAwaitable>);
static_assert(!HasProtocolRecvAwaitable<DefaultMysqlPrepareAwaitable>);

static_assert(!std::derived_from<DefaultMysqlStmtExecuteAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<DefaultMysqlStmtExecuteAwaitable>);
static_assert(!HasProtocolRecvAwaitable<DefaultMysqlStmtExecuteAwaitable>);

static_assert(!std::derived_from<DefaultMysqlPipelineAwaitable, galay::kernel::SequenceAwaitableBase>);
static_assert(!HasProtocolSendAwaitable<DefaultMysqlPipelineAwaitable>);
static_assert(!HasProtocolRecvAwaitable<DefaultMysqlPipelineAwaitable>);

static_assert(requires(DefaultMysqlClient& client, MysqlConfig config) {
    { client.connect(config) } -> std::same_as<DefaultMysqlConnectAwaitable>;
    { client.query("SELECT 1") } -> std::same_as<DefaultMysqlQueryAwaitable>;
    { client.prepare("SELECT ?") } -> std::same_as<DefaultMysqlPrepareAwaitable>;
    { client.stmtExecute(1u,
                         std::declval<std::span<const std::optional<std::string>>>(),
                         std::declval<std::span<const uint8_t>>()) } -> std::same_as<DefaultMysqlStmtExecuteAwaitable>;
    { client.batch(std::declval<std::span<const protocol::MysqlCommandView>>()) } -> std::same_as<DefaultMysqlPipelineAwaitable>;
});

static_assert(requires(VectorMysqlClient& client, MysqlConfig config) {
    { client.connect(config) } -> std::same_as<VectorMysqlConnectAwaitable>;
    { client.query("SELECT 1") } -> std::same_as<VectorMysqlQueryAwaitable>;
    { client.prepare("SELECT ?") } -> std::same_as<VectorMysqlPrepareAwaitable>;
    { client.stmtExecute(1u,
                         std::declval<std::span<const std::optional<std::string>>>(),
                         std::declval<std::span<const uint8_t>>()) } -> std::same_as<VectorMysqlStmtExecuteAwaitable>;
    { client.batch(std::declval<std::span<const protocol::MysqlCommandView>>()) } -> std::same_as<VectorMysqlPipelineAwaitable>;
    { client.ringBuffer() } -> std::same_as<RingBuffer<RingBufferBackendStrategy::Vector>&>;
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

    DefaultMysqlClient async_client(nullptr);
    auto async_query = async_client.query(oversized_sql);
    require(async_query.isInvalid(), "oversized async query awaitable should be invalid");
    auto async_pipeline = async_client.pipeline(oversized_pipeline);
    require(async_pipeline.isInvalid(), "oversized async pipeline awaitable should be invalid");

    return 0;
}
