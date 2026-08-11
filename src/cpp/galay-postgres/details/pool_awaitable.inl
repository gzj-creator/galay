#ifndef GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_INL
#define GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_INL

PostgresConnectionPool::AcquireAwaitable::AcquireAwaitable(PostgresConnectionPool& pool)
    : m_pool(pool)
{
}

std::expected<std::optional<AsyncPostgresClient<>*>, PostgresError>
PostgresConnectionPool::AcquireAwaitable::await_resume()
{
    if (m_state == State::Ready) {
        m_state = State::Invalid;
        return m_client;
    }

    if (m_state == State::Creating) {
        if (!m_connect_awaitable.has_value()) {
            m_state = State::Invalid;
            m_pool.recycleDisconnected(std::exchange(m_client, nullptr));
            return std::unexpected(PostgresError(
                POSTGRES_ERROR_INTERNAL,
                "Missing connect awaitable while creating a pool connection"));
        }

        auto connected = m_connect_awaitable->await_resume();
        m_connect_awaitable.reset();
        m_state = State::Invalid;
        if (!connected) {
            m_pool.recycleDisconnected(std::exchange(m_client, nullptr));
            return std::unexpected(connected.error());
        }
        if (!connected->has_value()) {
            m_pool.recycleDisconnected(std::exchange(m_client, nullptr));
            return std::unexpected(PostgresError(
                POSTGRES_ERROR_INTERNAL,
                "Connect awaitable resumed without a final value"));
        }
        return m_client;
    }

    if (m_state == State::Waiting) {
        m_client = m_waiter == nullptr
            ? nullptr
            : m_waiter->client.load(std::memory_order_acquire);
        m_waiter.reset();
        m_state = State::Invalid;
        if (m_client != nullptr) {
            return m_client;
        }
        return std::unexpected(PostgresError(
            POSTGRES_ERROR_INTERNAL,
            "Pool waiter resumed without a connection"));
    }

    if (m_state == State::EnqueueFailed) {
        m_waiter.reset();
        m_state = State::Invalid;
        return std::unexpected(PostgresError(
            POSTGRES_ERROR_INTERNAL,
            "Failed to enqueue PostgreSQL pool waiter"));
    }

    return std::unexpected(PostgresError(POSTGRES_ERROR_INTERNAL,
                                         "Invalid PostgreSQL pool acquire state"));
}

PostgresConnectionPool::LeaseAwaitable::LeaseAwaitable(PostgresConnectionPool& pool)
    : m_pool(pool)
    , m_acquire(pool)
{
}

std::expected<std::optional<PostgresPoolLease>, PostgresError>
PostgresConnectionPool::LeaseAwaitable::await_resume()
{
    auto acquired = m_acquire.await_resume();
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    if (!acquired->has_value()) {
        return std::optional<PostgresPoolLease>{};
    }
    std::optional<PostgresPoolLease> lease;
    lease.emplace(PostgresPoolLease(&m_pool, acquired->value()));
    return lease;
}

#endif // GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_INL
