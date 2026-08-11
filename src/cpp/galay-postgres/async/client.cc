#include "client.h"
#include "../details/awaitable.inl"

#include <utility>

namespace galay::postgres
{

AsyncPostgresClientBuilder&
AsyncPostgresClientBuilder::scheduler(galay::kernel::IOScheduler* scheduler) noexcept
{
    m_scheduler = scheduler;
    return *this;
}

AsyncPostgresClientBuilder&
AsyncPostgresClientBuilder::config(AsyncPostgresConfig config) noexcept
{
    m_config = std::move(config);
    return *this;
}

AsyncPostgresClientBuilder&
AsyncPostgresClientBuilder::sendTimeout(std::chrono::milliseconds timeout) noexcept
{
    m_config.send_timeout = timeout;
    return *this;
}

AsyncPostgresClientBuilder&
AsyncPostgresClientBuilder::recvTimeout(std::chrono::milliseconds timeout) noexcept
{
    m_config.recv_timeout = timeout;
    return *this;
}

AsyncPostgresClientBuilder& AsyncPostgresClientBuilder::bufferSize(size_t size) noexcept
{
    m_config.buffer_size = size;
    return *this;
}

AsyncPostgresClientBuilder&
AsyncPostgresClientBuilder::resultRowReserveHint(size_t hint) noexcept
{
    m_config.result_row_reserve_hint = hint;
    return *this;
}

AsyncPostgresClientBuilder& AsyncPostgresClientBuilder::tcpNoDelay(bool enabled) noexcept
{
    m_config.tcp_no_delay = enabled;
    return *this;
}

AsyncPostgresClient<> AsyncPostgresClientBuilder::build() const
{
    return AsyncPostgresClient<>(m_scheduler, m_config);
}

template<RingBufferBackendStrategy Strategy>
AsyncPostgresClient<Strategy>::AsyncPostgresClient(galay::kernel::IOScheduler* scheduler,
                                                   AsyncPostgresConfig config)
    : m_ring_buffer(config.buffer_size)
    , m_config(std::move(config))
    , m_scheduler(scheduler)
{
}

template<RingBufferBackendStrategy Strategy>
AsyncPostgresClient<Strategy>::AsyncPostgresClient(AsyncPostgresClient&& other) noexcept
    : m_socket(std::move(other.m_socket))
    , m_ring_buffer(std::move(other.m_ring_buffer))
    , m_server_parameters(std::move(other.m_server_parameters))
    , m_config(std::move(other.m_config))
    , m_backend_key_data(std::move(other.m_backend_key_data))
    , m_scheduler(other.m_scheduler)
    , m_parser(std::move(other.m_parser))
    , m_encoder(std::move(other.m_encoder))
    , m_transaction_status(other.m_transaction_status)
    , m_is_closed(other.m_is_closed)
{
    other.m_scheduler = nullptr;
    other.m_transaction_status = 'I';
    other.m_is_closed = true;
}

template<RingBufferBackendStrategy Strategy>
AsyncPostgresClient<Strategy>&
AsyncPostgresClient<Strategy>::operator=(AsyncPostgresClient&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    m_socket = std::move(other.m_socket);
    m_ring_buffer = std::move(other.m_ring_buffer);
    m_server_parameters = std::move(other.m_server_parameters);
    m_config = std::move(other.m_config);
    m_backend_key_data = std::move(other.m_backend_key_data);
    m_scheduler = other.m_scheduler;
    m_parser = std::move(other.m_parser);
    m_encoder = std::move(other.m_encoder);
    m_transaction_status = other.m_transaction_status;
    m_is_closed = other.m_is_closed;

    other.m_scheduler = nullptr;
    other.m_transaction_status = 'I';
    other.m_is_closed = true;
    return *this;
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::ConnectAwaitable
AsyncPostgresClient<Strategy>::connect(PostgresConfig config)
{
    return ConnectAwaitable(*this, std::move(config));
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::ConnectAwaitable
AsyncPostgresClient<Strategy>::connect(std::string_view host,
                                       uint16_t port,
                                       std::string_view user,
                                       std::string_view password,
                                       std::string_view database)
{
    PostgresConfig config;
    config.host.assign(host);
    config.port = port;
    config.username.assign(user);
    config.password.assign(password);
    config.database.assign(database);
    config.tcp_no_delay = m_config.tcp_no_delay;
    return connect(std::move(config));
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::QueryAwaitable
AsyncPostgresClient<Strategy>::query(std::string_view sql)
{
    return QueryAwaitable(*this, sql);
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::PipelineAwaitable
AsyncPostgresClient<Strategy>::batch(
    std::span<const protocol::PostgresCommandView> commands)
{
    return PipelineAwaitable(*this, commands);
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::PipelineAwaitable
AsyncPostgresClient<Strategy>::pipeline(std::span<const std::string_view> sqls)
{
    size_t encoded_bytes = 0;
    for (std::string_view sql : sqls) {
        encoded_bytes += protocol::kMessageHeaderSize + sql.size() + 1;
    }

    protocol::PostgresCommandBuilder builder;
    builder.reserve(sqls.size(), encoded_bytes);
    for (std::string_view sql : sqls) {
        builder.appendQuery(sql);
    }
    return batch(builder.commands());
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::PrepareAwaitable
AsyncPostgresClient<Strategy>::prepare(std::string_view name,
                                       std::string_view sql,
                                       std::span<const uint32_t> parameter_types)
{
    return PrepareAwaitable(*this, name, sql, parameter_types);
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::ExecuteAwaitable
AsyncPostgresClient<Strategy>::execute(
    std::string_view name,
    std::span<const std::optional<std::string>> params)
{
    std::vector<std::optional<std::string_view>> views;
    views.reserve(params.size());
    for (const auto& param : params) {
        if (param.has_value()) {
            views.emplace_back(*param);
        } else {
            views.push_back(std::nullopt);
        }
    }
    return ExecuteAwaitable(*this, name, views);
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::ExecuteAwaitable
AsyncPostgresClient<Strategy>::execute(
    std::string_view name,
    std::span<const std::optional<std::string_view>> params)
{
    return ExecuteAwaitable(*this, name, params);
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::QueryAwaitable
AsyncPostgresClient<Strategy>::beginTransaction()
{
    return query("BEGIN");
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::QueryAwaitable AsyncPostgresClient<Strategy>::commit()
{
    return query("COMMIT");
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::QueryAwaitable AsyncPostgresClient<Strategy>::rollback()
{
    return query("ROLLBACK");
}

template<RingBufferBackendStrategy Strategy>
typename AsyncPostgresClient<Strategy>::QueryAwaitable AsyncPostgresClient<Strategy>::ping()
{
    return query("SELECT 1");
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::Task<PostgresVoidResult> AsyncPostgresClient<Strategy>::close()
{
    if (m_is_closed) {
        co_return PostgresVoidResult{};
    }

    std::optional<PostgresError> first_error;
    const std::string terminate = m_encoder.encodeTerminate();
    size_t sent = 0;
    while (sent < terminate.size()) {
        std::expected<size_t, galay::kernel::IOError> send_result =
            std::unexpected(galay::kernel::IOError(galay::kernel::kNotReady, 0));
        if (m_config.isSendTimeoutEnabled()) {
            send_result = co_await m_socket.send(terminate.data() + sent,
                                                 terminate.size() - sent)
                              .timeout(m_config.send_timeout);
        } else {
            send_result = co_await m_socket.send(terminate.data() + sent,
                                                 terminate.size() - sent);
        }

        if (!send_result) {
            if (galay::kernel::IOError::contains(send_result.error().code(),
                                                 galay::kernel::kNotReady) ||
                galay::kernel::IOError::contains(
                    send_result.error().code(), galay::kernel::kNotRunningOnIOScheduler)) {
                co_return std::unexpected(PostgresError(
                    POSTGRES_ERROR_INTERNAL,
                    "PostgreSQL close conflicts with an in-flight operation"));
            }
            const PostgresErrorType type = galay::kernel::IOError::contains(
                send_result.error().code(), galay::kernel::kTimeout)
                ? POSTGRES_ERROR_TIMEOUT
                : POSTGRES_ERROR_SEND;
            first_error.emplace(type, send_result.error().message());
            break;
        }
        if (*send_result == 0) {
            first_error.emplace(POSTGRES_ERROR_CONNECTION_CLOSED,
                                "Connection closed while sending Terminate");
            break;
        }
        sent += *send_result;
    }

    auto close_result = co_await m_socket.close();
    m_is_closed = true;
    if (!close_result && !first_error.has_value()) {
        first_error.emplace(POSTGRES_ERROR_CONNECTION, close_result.error().message());
    }
    if (first_error.has_value()) {
        co_return std::unexpected(std::move(*first_error));
    }
    co_return PostgresVoidResult{};
}

template<RingBufferBackendStrategy Strategy>
void AsyncPostgresClient<Strategy>::setServerParameter(std::string name, std::string value)
{
    m_server_parameters.insert_or_assign(std::move(name), std::move(value));
}

template<RingBufferBackendStrategy Strategy>
void AsyncPostgresClient<Strategy>::setBackendKeyData(
    protocol::BackendKeyDataInfo data) noexcept
{
    m_backend_key_data = data;
}

template class details::PostgresConnectAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::PostgresConnectAwaitable<RingBufferBackendStrategy::Vector>;
template class details::PostgresConnectAwaitable<RingBufferBackendStrategy::Auto>;
template class details::PostgresQueryAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::PostgresQueryAwaitable<RingBufferBackendStrategy::Vector>;
template class details::PostgresQueryAwaitable<RingBufferBackendStrategy::Auto>;
template class details::PostgresPrepareAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::PostgresPrepareAwaitable<RingBufferBackendStrategy::Vector>;
template class details::PostgresPrepareAwaitable<RingBufferBackendStrategy::Auto>;
template class details::PostgresExecuteAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::PostgresExecuteAwaitable<RingBufferBackendStrategy::Vector>;
template class details::PostgresExecuteAwaitable<RingBufferBackendStrategy::Auto>;
template class details::PostgresPipelineAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::PostgresPipelineAwaitable<RingBufferBackendStrategy::Vector>;
template class details::PostgresPipelineAwaitable<RingBufferBackendStrategy::Auto>;
template class AsyncPostgresClient<RingBufferBackendStrategy::Mmap>;
template class AsyncPostgresClient<RingBufferBackendStrategy::Vector>;
template class AsyncPostgresClient<RingBufferBackendStrategy::Auto>;

} // namespace galay::postgres
