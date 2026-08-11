#ifndef GALAY_POSTGRES_ASYNC_CLIENT_H
#define GALAY_POSTGRES_ASYNC_CLIENT_H

#include "../base/postgres_config.h"
#include "../base/postgres_error.h"
#include "../base/postgres_value.h"
#include "../protoc/builder.h"
#include "../protoc/postgres_auth.h"
#include "../protoc/postgres_protocol.h"
#include "../../galay-kernel/async/async_tcp.h"
#include "../../galay-kernel/common/host.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/task.h"
#include "../../galay-kernel/core/timeout.hpp"
#include "../../galay-utils/cache/ring_buffer.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::postgres
{

using PostgresResult = std::expected<PostgresResultSet, PostgresError>;
using PostgresVoidResult = std::expected<void, PostgresError>;
using PostgresBatchResult = std::expected<std::vector<PostgresResultSet>, PostgresError>;

using galay::utils::RingBuffer;
using galay::utils::RingBufferBackendStrategy;

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class AsyncPostgresClient;

namespace details
{
template<RingBufferBackendStrategy Strategy> class PostgresConnectAwaitable;
template<RingBufferBackendStrategy Strategy> class PostgresQueryAwaitable;
template<RingBufferBackendStrategy Strategy> class PostgresPrepareAwaitable;
template<RingBufferBackendStrategy Strategy> class PostgresExecuteAwaitable;
template<RingBufferBackendStrategy Strategy> class PostgresPipelineAwaitable;
} // namespace details

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using PostgresConnectAwaitable = details::PostgresConnectAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using PostgresQueryAwaitable = details::PostgresQueryAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using PostgresPrepareAwaitable = details::PostgresPrepareAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using PostgresExecuteAwaitable = details::PostgresExecuteAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using PostgresPipelineAwaitable = details::PostgresPipelineAwaitable<Strategy>;

class AsyncPostgresClientBuilder
{
public:
    AsyncPostgresClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler) noexcept;
    AsyncPostgresClientBuilder& config(AsyncPostgresConfig config) noexcept;
    AsyncPostgresClientBuilder& sendTimeout(std::chrono::milliseconds timeout) noexcept;
    AsyncPostgresClientBuilder& recvTimeout(std::chrono::milliseconds timeout) noexcept;
    AsyncPostgresClientBuilder& bufferSize(size_t size) noexcept;
    AsyncPostgresClientBuilder& resultRowReserveHint(size_t hint) noexcept;
    AsyncPostgresClientBuilder& tcpNoDelay(bool enabled) noexcept;

    [[nodiscard]] AsyncPostgresClient<> build() const;
    [[nodiscard]] AsyncPostgresConfig buildConfig() const { return m_config; }

private:
    galay::kernel::IOScheduler* m_scheduler = nullptr;
    AsyncPostgresConfig m_config = AsyncPostgresConfig::noTimeout();
};

/**
 * @brief PostgreSQL wire-protocol v3 asynchronous client.
 * @note The client is move-only and permits one in-flight operation at a time.
 *       Awaitables own encoded input until their I/O completes.
 */
template<RingBufferBackendStrategy Strategy>
class AsyncPostgresClient
{
public:
    using ConnectAwaitable = details::PostgresConnectAwaitable<Strategy>;
    using QueryAwaitable = details::PostgresQueryAwaitable<Strategy>;
    using PrepareAwaitable = details::PostgresPrepareAwaitable<Strategy>;
    using ExecuteAwaitable = details::PostgresExecuteAwaitable<Strategy>;
    using PipelineAwaitable = details::PostgresPipelineAwaitable<Strategy>;

    explicit AsyncPostgresClient(
        galay::kernel::IOScheduler* scheduler,
        AsyncPostgresConfig config = AsyncPostgresConfig::noTimeout());
    AsyncPostgresClient(AsyncPostgresClient&& other) noexcept;
    AsyncPostgresClient& operator=(AsyncPostgresClient&& other) noexcept;
    AsyncPostgresClient(const AsyncPostgresClient&) = delete;
    AsyncPostgresClient& operator=(const AsyncPostgresClient&) = delete;
    ~AsyncPostgresClient() = default;

    ConnectAwaitable connect(PostgresConfig config);
    ConnectAwaitable connect(std::string_view host,
                             uint16_t port,
                             std::string_view user,
                             std::string_view password,
                             std::string_view database = {});

    QueryAwaitable query(std::string_view sql);
    PipelineAwaitable batch(std::span<const protocol::PostgresCommandView> commands);
    PipelineAwaitable pipeline(std::span<const std::string_view> sqls);

    PrepareAwaitable prepare(std::string_view name,
                             std::string_view sql,
                             std::span<const uint32_t> parameter_types = {});
    ExecuteAwaitable execute(std::string_view name,
                             std::span<const std::optional<std::string>> params);
    ExecuteAwaitable execute(std::string_view name,
                             std::span<const std::optional<std::string_view>> params);

    QueryAwaitable beginTransaction();
    QueryAwaitable commit();
    QueryAwaitable rollback();
    QueryAwaitable ping();

    galay::kernel::Task<PostgresVoidResult> close();
    [[nodiscard]] bool isClosed() const noexcept { return m_is_closed; }
    [[nodiscard]] char transactionStatus() const noexcept { return m_transaction_status; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& serverParameters() const noexcept
    {
        return m_server_parameters;
    }
    [[nodiscard]] const std::optional<protocol::BackendKeyDataInfo>& backendKeyData() const noexcept
    {
        return m_backend_key_data;
    }

    galay::async::AsyncTcpSocket& socket() noexcept { return m_socket; }
    RingBuffer<Strategy, std::dynamic_extent>& ringBuffer() noexcept { return m_ring_buffer; }
    const RingBuffer<Strategy, std::dynamic_extent>& ringBuffer() const noexcept { return m_ring_buffer; }
    protocol::PostgresParser& parser() noexcept { return m_parser; }
    protocol::PostgresEncoder& encoder() noexcept { return m_encoder; }
    const AsyncPostgresConfig& asyncConfig() const noexcept { return m_config; }

    void setTransactionStatus(char status) noexcept { m_transaction_status = status; }
    void setServerParameter(std::string name, std::string value);
    void setBackendKeyData(protocol::BackendKeyDataInfo data) noexcept;
    void setClosed(bool closed) noexcept { m_is_closed = closed; }

private:
    friend class details::PostgresConnectAwaitable<Strategy>;
    friend class details::PostgresQueryAwaitable<Strategy>;
    friend class details::PostgresPrepareAwaitable<Strategy>;
    friend class details::PostgresExecuteAwaitable<Strategy>;
    friend class details::PostgresPipelineAwaitable<Strategy>;

    galay::async::AsyncTcpSocket m_socket;
    RingBuffer<Strategy, std::dynamic_extent> m_ring_buffer;
    std::unordered_map<std::string, std::string> m_server_parameters;
    AsyncPostgresConfig m_config;
    std::optional<protocol::BackendKeyDataInfo> m_backend_key_data;
    galay::kernel::IOScheduler* m_scheduler = nullptr;
    protocol::PostgresParser m_parser;
    protocol::PostgresEncoder m_encoder;
    char m_transaction_status = 'I';
    bool m_is_closed = true;
};

} // namespace galay::postgres

#include "../details/awaitable.h"

#endif // GALAY_POSTGRES_ASYNC_CLIENT_H
