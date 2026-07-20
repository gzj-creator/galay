#ifndef GALAY_MYSQL_DETAILS_POOL_AWAITABLE_INL
#define GALAY_MYSQL_DETAILS_POOL_AWAITABLE_INL

/**
 * @file pool_awaitable.inl
 * @brief MySQL 连接池等待体实现
 * @details 仅由 async/conn_pool.cc 包含。
 */

// ======================== AcquireAwaitable ========================

MysqlConnectionPool::AcquireAwaitable::AcquireAwaitable(MysqlConnectionPool& pool)
    : m_pool(pool)
{
}

bool MysqlConnectionPool::AcquireAwaitable::await_ready() const noexcept
{
    return false;
}

std::expected<std::optional<AsyncMysqlClient<>*>, MysqlError>
MysqlConnectionPool::AcquireAwaitable::await_resume()
{
    if (m_state == State::Ready) {
        m_state = State::Invalid;
        m_connect_awaitable.reset();
        return m_client;
    }
    else if (m_state == State::Creating) {
        if (!m_connect_awaitable.has_value()) {
            m_state = State::Invalid;
            m_client = nullptr;
            return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Missing connect awaitable in creating state"));
        }

        auto result = m_connect_awaitable.value().await_resume();
        m_connect_awaitable.reset();

        if (!result) {
            m_state = State::Invalid;
            m_client = nullptr;
            return std::unexpected(result.error());
        }
        if (!result->has_value()) {
            m_state = State::Invalid;
            m_client = nullptr;
            return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Connect awaitable resumed without value"));
        }
        m_state = State::Invalid;
        return m_client;
    }
    else if (m_state == State::Waiting) {
        m_client = m_waiter ? m_waiter->client.load(std::memory_order_acquire) : nullptr;
        m_state = State::Invalid;
        m_waiter.reset();
        m_connect_awaitable.reset();
        if (m_client) {
            return m_client;
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Failed to acquire connection after wakeup"));
    }
    else if (m_state == State::EnqueueFailed) {
        m_state = State::Invalid;
        m_waiter.reset();
        m_connect_awaitable.reset();
        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Failed to enqueue connection waiter"));
    }

    m_state = State::Invalid;
    m_waiter.reset();
    m_connect_awaitable.reset();
    m_client = nullptr;
    return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid acquire state"));
}

// ======================== LeaseAwaitable ========================

MysqlConnectionPool::LeaseAwaitable::LeaseAwaitable(MysqlConnectionPool& pool)
    : m_pool(pool)
    , m_acquire(pool)
{
}

bool MysqlConnectionPool::LeaseAwaitable::await_ready() const noexcept
{
    return m_acquire.await_ready();
}

std::expected<std::optional<MysqlPoolLease>, MysqlError>
MysqlConnectionPool::LeaseAwaitable::await_resume()
{
    auto acquired = m_acquire.await_resume();
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    if (!acquired->has_value()) {
        return std::optional<MysqlPoolLease>{};
    }
    std::optional<MysqlPoolLease> lease;
    lease.emplace(MysqlPoolLease(&m_pool, acquired->value()));
    return lease;
}

#endif // GALAY_MYSQL_DETAILS_POOL_AWAITABLE_INL
