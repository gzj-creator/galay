#include "conn_pool.h"

#include <utility>

namespace galay::mysql
{

// ======================== MysqlConnectionPool ========================

MysqlConnectionPool::MysqlConnectionPool(galay::kernel::IOScheduler* scheduler,
                                         MysqlConnectionPoolConfig config)
    : m_scheduler(scheduler)
    , m_mysql_config(std::move(config.mysql_config))
    , m_async_config(std::move(config.async_config))
    , m_min_connections(config.min_connections)
    , m_max_connections(config.max_connections)
{
}

MysqlConnectionPool::~MysqlConnectionPool()
{
    std::queue<std::coroutine_handle<>> waiters_to_resume;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        waiters_to_resume.swap(m_waiters);
        while (!m_idle_clients.empty()) {
            m_idle_clients.pop();
        }
        m_all_clients.clear();
    }
    while (!waiters_to_resume.empty()) {
        auto h = waiters_to_resume.front();
        waiters_to_resume.pop();
        h.resume();
    }
}

AsyncMysqlClient* MysqlConnectionPool::tryAcquire()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_idle_clients.empty()) {
        auto* client = m_idle_clients.front();
        m_idle_clients.pop();
        return client;
    }
    return nullptr;
}

AsyncMysqlClient* MysqlConnectionPool::createClient()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_total_connections.load(std::memory_order_relaxed) >= m_max_connections) {
        return nullptr;
    }
    auto client = std::make_unique<AsyncMysqlClient>(m_scheduler, m_async_config);
    auto* ptr = client.get();
    m_all_clients.push_back(std::move(client));
    m_total_connections.fetch_add(1, std::memory_order_relaxed);
    return ptr;
}

void MysqlConnectionPool::release(AsyncMysqlClient* client)
{
    if (!client) return;

    std::coroutine_handle<> waiter_to_resume;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_waiters.empty()) {
            waiter_to_resume = m_waiters.front();
            m_waiters.pop();
        }
        m_idle_clients.push(client);
    }
    if (waiter_to_resume) {
        waiter_to_resume.resume();
    }
}

size_t MysqlConnectionPool::idleCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_idle_clients.size();
}

MysqlConnectionPool::AcquireAwaitable MysqlConnectionPool::acquire() { return AcquireAwaitable(*this); }

// ======================== AcquireAwaitable ========================

MysqlConnectionPool::AcquireAwaitable::AcquireAwaitable(MysqlConnectionPool& pool)
    : m_pool(pool)
    , m_state(State::Invalid)
{
}

bool MysqlConnectionPool::AcquireAwaitable::await_ready() const noexcept
{
    return false;
}

std::expected<std::optional<AsyncMysqlClient*>, MysqlError>
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
        // 被唤醒后，从池中获取连接
        m_client = m_pool.tryAcquire();
        m_state = State::Invalid;
        m_connect_awaitable.reset();
        if (m_client) {
            return m_client;
        }
        return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Failed to acquire connection after wakeup"));
    }

    m_state = State::Invalid;
    m_connect_awaitable.reset();
    m_client = nullptr;
    return std::unexpected(MysqlError(MYSQL_ERROR_INTERNAL, "Invalid acquire state"));
}

} // namespace galay::mysql
