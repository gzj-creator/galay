#include <galay/cpp/galay-mysql/async/client.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <concepts>
#include <expected>
#include <type_traits>
#include <utility>

using namespace galay::mysql;

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
    return 0;
}
