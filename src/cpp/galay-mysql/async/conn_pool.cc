#include "conn_pool.h"

#include <utility>

namespace galay::mysql
{

// ======================== MysqlPoolLease ========================

MysqlPoolLease::MysqlPoolLease() noexcept = default;

MysqlPoolLease::MysqlPoolLease(MysqlConnectionPool* pool, AsyncMysqlClient<>* client) noexcept
    : m_pool(pool)
    , m_client(client)
{
}

MysqlPoolLease::MysqlPoolLease(MysqlPoolLease&& other) noexcept
    : m_pool(std::exchange(other.m_pool, nullptr))
    , m_client(std::exchange(other.m_client, nullptr))
{
}

MysqlPoolLease& MysqlPoolLease::operator=(MysqlPoolLease&& other) noexcept
{
    if (this != &other) {
        release();
        m_pool = std::exchange(other.m_pool, nullptr);
        m_client = std::exchange(other.m_client, nullptr);
    }
    return *this;
}

MysqlPoolLease::~MysqlPoolLease()
{
    release();
}

AsyncMysqlClient<>* MysqlPoolLease::get() const noexcept
{
    return m_client;
}

AsyncMysqlClient<>& MysqlPoolLease::operator*() const noexcept
{
    return *m_client;
}

AsyncMysqlClient<>* MysqlPoolLease::operator->() const noexcept
{
    return m_client;
}

MysqlPoolLease::operator bool() const noexcept
{
    return m_client != nullptr;
}

void MysqlPoolLease::release() noexcept
{
    auto* pool = std::exchange(m_pool, nullptr);
    auto* client = std::exchange(m_client, nullptr);
    if (pool != nullptr && client != nullptr) {
        pool->release(client);
    }
}

AsyncMysqlClient<>* MysqlPoolLease::dismiss() noexcept
{
    m_pool = nullptr;
    return std::exchange(m_client, nullptr);
}

// ======================== MysqlConnectionPool ========================

MysqlConnectionPool::MysqlConnectionPool(galay::kernel::IOScheduler* scheduler,
                                         MysqlConnectionPoolConfig config)
    : m_scheduler(scheduler)
    , m_mysql_config(std::move(config.mysql_config))
    , m_async_config(std::move(config.async_config))
    , m_min_connections(config.min_connections)
    , m_max_connections(config.max_connections)
    , m_idle_clients(m_max_connections == 0 ? 1 : m_max_connections)
    , m_waiters(m_max_connections == 0 ? 1 : m_max_connections)
    , m_all_clients(m_max_connections)
{
    (void)m_min_connections;
}

MysqlConnectionPool::~MysqlConnectionPool()
{
    std::shared_ptr<detail::MysqlPoolWaiter> waiter;
    while (m_waiters.try_dequeue(waiter)) {
        if (waiter != nullptr) {
            waiter->active.store(false, std::memory_order_release);
            waiter->client.store(nullptr, std::memory_order_release);
            waiter->waker.wakeUp();
        }
    }
    AsyncMysqlClient<>* client = nullptr;
    while (m_idle_clients.try_dequeue(client)) {}
}

AsyncMysqlClient<>* MysqlConnectionPool::tryAcquire()
{
    AsyncMysqlClient<>* client = nullptr;
    if (m_idle_clients.try_dequeue(client)) {
        m_idle_connections.fetch_sub(1, std::memory_order_acq_rel);
        return client;
    }
    return nullptr;
}

AsyncMysqlClient<>* MysqlConnectionPool::createClient()
{
    size_t slot = m_total_connections.load(std::memory_order_acquire);
    while (slot < m_max_connections) {
        if (m_total_connections.compare_exchange_weak(slot,
                                                       slot + 1,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
            auto client = std::make_unique<AsyncMysqlClient<>>(m_scheduler, m_async_config);
            auto* ptr = client.get();
            m_all_clients[slot] = std::move(client);
            return ptr;
        }
    }
    return nullptr;
}

bool MysqlConnectionPool::enqueueWaiter(std::shared_ptr<detail::MysqlPoolWaiter> waiter)
{
    return waiter != nullptr && m_waiters.enqueue(std::move(waiter));
}

bool MysqlConnectionPool::wakeOneWaiter()
{
    std::shared_ptr<detail::MysqlPoolWaiter> waiter;
    while (m_idle_connections.load(std::memory_order_acquire) > 0 &&
           m_waiters.try_dequeue(waiter)) {
        if (waiter == nullptr) {
            continue;
        }

        bool expected = true;
        if (!waiter->active.compare_exchange_strong(expected,
                                                    false,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            continue;
        }

        auto* client = tryAcquire();
        if (client == nullptr) {
            waiter->active.store(true, std::memory_order_release);
            auto requeued_waiter = waiter;
            if (!enqueueWaiter(std::move(waiter)) && requeued_waiter != nullptr) {
                requeued_waiter->active.store(false, std::memory_order_release);
                requeued_waiter->waker.wakeUp();
            }
            return false;
        }

        waiter->client.store(client, std::memory_order_release);
        waiter->waker.wakeUp();
        return true;
    }

    return false;
}

void MysqlConnectionPool::release(AsyncMysqlClient<>* client)
{
    if (!client) return;

    if (!m_idle_clients.enqueue(client)) {
        return;
    }
    m_idle_connections.fetch_add(1, std::memory_order_acq_rel);
    (void)wakeOneWaiter();
}

size_t MysqlConnectionPool::idleCount() const
{
    return m_idle_connections.load(std::memory_order_acquire);
}

MysqlConnectionPool::AcquireAwaitable MysqlConnectionPool::acquire() { return AcquireAwaitable(*this); }

MysqlConnectionPool::LeaseAwaitable MysqlConnectionPool::acquireLease() { return LeaseAwaitable(*this); }

#include "../details/pool_awaitable.inl"

} // namespace galay::mysql
