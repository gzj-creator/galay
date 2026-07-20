#include "client.h"
#include "../details/awaitable.inl"

namespace galay::mysql
{

// ======================== AsyncMysqlClient<Strategy> 实现 ========================

template<RingBufferBackendStrategy Strategy>
AsyncMysqlClient<Strategy>::AsyncMysqlClient(IOScheduler* scheduler,
                                   AsyncMysqlConfig config)
    : m_scheduler(scheduler)
    , m_config(std::move(config))
    , m_ring_buffer(m_config.buffer_size)
{
}

template<RingBufferBackendStrategy Strategy>
AsyncMysqlClient<Strategy>::AsyncMysqlClient(AsyncMysqlClient<Strategy>&& other) noexcept
    : m_socket(std::move(other.m_socket))
    , m_scheduler(other.m_scheduler)
    , m_config(std::move(other.m_config))
    , m_ring_buffer(std::move(other.m_ring_buffer))
    , m_server_capabilities(other.m_server_capabilities)
    , m_parser(std::move(other.m_parser))
    , m_encoder(std::move(other.m_encoder))
    , m_is_closed(other.m_is_closed)
{
    other.m_is_closed = true;
}

template<RingBufferBackendStrategy Strategy>
AsyncMysqlClient<Strategy>& AsyncMysqlClient<Strategy>::operator=(AsyncMysqlClient<Strategy>&& other) noexcept
{
    if (this != &other) {
        m_socket = std::move(other.m_socket);
        m_scheduler = other.m_scheduler;
        m_config = std::move(other.m_config);
        m_ring_buffer = std::move(other.m_ring_buffer);
        m_server_capabilities = other.m_server_capabilities;
        m_parser = std::move(other.m_parser);
        m_encoder = std::move(other.m_encoder);
        m_is_closed = other.m_is_closed;
        other.m_is_closed = true;
    }
    return *this;
}

template<RingBufferBackendStrategy Strategy>
MysqlConnectAwaitable<Strategy> AsyncMysqlClient<Strategy>::connect(MysqlConfig config)
{
    return MysqlConnectAwaitable<Strategy>(*this, std::move(config));
}

template<RingBufferBackendStrategy Strategy>
MysqlConnectAwaitable<Strategy> AsyncMysqlClient<Strategy>::connect(std::string_view host, uint16_t port,
                                                std::string_view user, std::string_view password,
                                                std::string_view database)
{
    MysqlConfig config;
    config.host.assign(host.data(), host.size());
    config.port = port;
    config.username.assign(user.data(), user.size());
    config.password.assign(password.data(), password.size());
    config.database.assign(database.data(), database.size());
    config.tcp_no_delay = m_config.tcp_no_delay;
    return connect(std::move(config));
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy> AsyncMysqlClient<Strategy>::query(std::string_view sql)
{
    return MysqlQueryAwaitable<Strategy>(*this, sql);
}

template<RingBufferBackendStrategy Strategy>
MysqlPipelineAwaitable<Strategy> AsyncMysqlClient<Strategy>::batch(std::span<const protocol::MysqlCommandView> commands)
{
    return MysqlPipelineAwaitable<Strategy>(*this, commands);
}

template<RingBufferBackendStrategy Strategy>
MysqlPipelineAwaitable<Strategy> AsyncMysqlClient<Strategy>::pipeline(std::span<const std::string_view> sqls)
{
    size_t reserve_bytes = 0;
    for (const auto sql : sqls) {
        reserve_bytes += protocol::MYSQL_PACKET_HEADER_SIZE + 1 + sql.size();
    }

    protocol::MysqlCommandBuilder builder;
    builder.reserve(sqls.size(), reserve_bytes);
    for (const auto sql : sqls) {
        builder.appendQuery(sql);
    }

    return batch(builder.commands());
}

template<RingBufferBackendStrategy Strategy>
MysqlPrepareAwaitable<Strategy> AsyncMysqlClient<Strategy>::prepare(std::string_view sql)
{
    return MysqlPrepareAwaitable<Strategy>(*this, sql);
}

template<RingBufferBackendStrategy Strategy>
MysqlStmtExecuteAwaitable<Strategy> AsyncMysqlClient<Strategy>::stmtExecute(uint32_t stmt_id,
                                                        std::span<const std::optional<std::string>> params,
                                                        std::span<const uint8_t> param_types)
{
    return MysqlStmtExecuteAwaitable<Strategy>(*this, m_encoder.encodeStmtExecute(stmt_id, params, param_types, 0));
}

template<RingBufferBackendStrategy Strategy>
MysqlStmtExecuteAwaitable<Strategy> AsyncMysqlClient<Strategy>::stmtExecute(uint32_t stmt_id,
                                                        std::span<const std::optional<std::string_view>> params,
                                                        std::span<const uint8_t> param_types)
{
    return MysqlStmtExecuteAwaitable<Strategy>(*this, m_encoder.encodeStmtExecute(stmt_id, params, param_types, 0));
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy> AsyncMysqlClient<Strategy>::beginTransaction()
{
    return query("BEGIN");
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy> AsyncMysqlClient<Strategy>::commit()
{
    return query("COMMIT");
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy> AsyncMysqlClient<Strategy>::rollback()
{
    return query("ROLLBACK");
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy> AsyncMysqlClient<Strategy>::ping()
{
    return query("SELECT 1");
}

template<RingBufferBackendStrategy Strategy>
MysqlQueryAwaitable<Strategy> AsyncMysqlClient<Strategy>::useDatabase(std::string_view database)
{
    std::string sql;
    sql.reserve(4 + database.size());
    sql.append("USE ");
    sql.append(database.data(), database.size());
    return query(sql);
}

template class details::MysqlConnectAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::MysqlConnectAwaitable<RingBufferBackendStrategy::Vector>;
template class details::MysqlConnectAwaitable<RingBufferBackendStrategy::Auto>;
template class details::MysqlQueryAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::MysqlQueryAwaitable<RingBufferBackendStrategy::Vector>;
template class details::MysqlQueryAwaitable<RingBufferBackendStrategy::Auto>;
template class details::MysqlPrepareAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::MysqlPrepareAwaitable<RingBufferBackendStrategy::Vector>;
template class details::MysqlPrepareAwaitable<RingBufferBackendStrategy::Auto>;
template class details::MysqlStmtExecuteAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::MysqlStmtExecuteAwaitable<RingBufferBackendStrategy::Vector>;
template class details::MysqlStmtExecuteAwaitable<RingBufferBackendStrategy::Auto>;
template class details::MysqlPipelineAwaitable<RingBufferBackendStrategy::Mmap>;
template class details::MysqlPipelineAwaitable<RingBufferBackendStrategy::Vector>;
template class details::MysqlPipelineAwaitable<RingBufferBackendStrategy::Auto>;
template class AsyncMysqlClient<RingBufferBackendStrategy::Mmap>;
template class AsyncMysqlClient<RingBufferBackendStrategy::Vector>;
template class AsyncMysqlClient<RingBufferBackendStrategy::Auto>;

} // namespace galay::mysql
