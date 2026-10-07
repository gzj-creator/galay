#include "conn_pool.h"

#include <algorithm>

namespace galay::postgres
{

PostgresPoolLease::PostgresPoolLease(PostgresConnectionPool* pool,
                                     AsyncPostgresClient<>* client) noexcept
    : m_pool(pool)
    , m_client(client)
{
}

PostgresPoolLease::PostgresPoolLease(PostgresPoolLease&& other) noexcept
    : m_pool(std::exchange(other.m_pool, nullptr))
    , m_client(std::exchange(other.m_client, nullptr))
{
}

PostgresPoolLease& PostgresPoolLease::operator=(PostgresPoolLease&& other) noexcept
{
    if (this != &other) {
        release();
        m_pool = std::exchange(other.m_pool, nullptr);
        m_client = std::exchange(other.m_client, nullptr);
    }
    return *this;
}

PostgresPoolLease::~PostgresPoolLease()
{
    release();
}

void PostgresPoolLease::release() noexcept
{
    auto* pool = std::exchange(m_pool, nullptr);
    auto* client = std::exchange(m_client, nullptr);
    if (pool != nullptr && client != nullptr) {
        pool->release(client);
    }
}

AsyncPostgresClient<>* PostgresPoolLease::dismiss() noexcept
{
    m_pool = nullptr;
    return std::exchange(m_client, nullptr);
}

PostgresConnectionPool::PostgresConnectionPool(galay::kernel::IOScheduler* scheduler,
                                               PostgresConnectionPoolConfig config)
    : m_scheduler(scheduler)
    , m_postgres_config(std::move(config.postgres_config))
    , m_async_config(std::move(config.async_config))
    , m_min_connections(std::min(config.min_connections, config.max_connections))
    , m_max_connections(config.max_connections)
    , m_idle_clients(m_max_connections == 0 ? 1 : m_max_connections)
    , m_disconnected_clients(m_max_connections == 0 ? 1 : m_max_connections)
    , m_waiters(m_max_connections == 0 ? 1 : m_max_connections)
    , m_all_clients(m_max_connections)
{
    for (size_t slot = 0; slot < m_min_connections; ++slot) {
        auto client = std::make_unique<AsyncPostgresClient<>>(m_scheduler, m_async_config);
        auto* pointer = client.get();
        m_all_clients[slot] = std::move(client);
        if (!m_disconnected_clients.enqueue(pointer)) {
            m_all_clients[slot].reset();
            break;
        }
        m_total_connections.fetch_add(1, std::memory_order_release);
    }
}

PostgresConnectionPool::~PostgresConnectionPool()
{
    std::shared_ptr<detail::PostgresPoolWaiter> waiter;
    while (m_waiters.try_dequeue(waiter)) {
        if (waiter == nullptr) {
            continue;
        }
        waiter->active.store(false, std::memory_order_release);
        waiter->client.store(nullptr, std::memory_order_release);
        waiter->waker.wake_up();
    }

    AsyncPostgresClient<>* ignored = nullptr;
    while (m_idle_clients.try_dequeue(ignored)) {}
    while (m_disconnected_clients.try_dequeue(ignored)) {}
}

AsyncPostgresClient<>* PostgresConnectionPool::try_acquire()
{
    AsyncPostgresClient<>* client = nullptr;
    if (!m_idle_clients.try_dequeue(client)) {
        return nullptr;
    }
    m_idle_connections.fetch_sub(1, std::memory_order_acq_rel);
    return client;
}

AsyncPostgresClient<>* PostgresConnectionPool::create_client()
{
    AsyncPostgresClient<>* recycled = nullptr;
    if (m_disconnected_clients.try_dequeue(recycled)) {
        return recycled;
    }

    size_t slot = m_total_connections.load(std::memory_order_acquire);
    while (slot < m_max_connections) {
        if (m_total_connections.compare_exchange_weak(slot,
                                                       slot + 1,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
            auto client = std::make_unique<AsyncPostgresClient<>>(m_scheduler, m_async_config);
            auto* pointer = client.get();
            m_all_clients[slot] = std::move(client);
            return pointer;
        }
    }
    return nullptr;
}

void PostgresConnectionPool::recycle_disconnected(AsyncPostgresClient<>* client)
{
    if (client == nullptr) {
        return;
    }
    *client = AsyncPostgresClient<>(m_scheduler, m_async_config);
    if (!m_disconnected_clients.enqueue(client)) {
        (void)fail_one_waiter();
    }
}

bool PostgresConnectionPool::enqueue_waiter(std::shared_ptr<detail::PostgresPoolWaiter> waiter)
{
    return waiter != nullptr && m_waiters.enqueue(std::move(waiter));
}

bool PostgresConnectionPool::wake_one_waiter()
{
    std::shared_ptr<detail::PostgresPoolWaiter> waiter;
    while (m_idle_connections.load(std::memory_order_acquire) > 0 &&
           m_waiters.try_dequeue(waiter)) {
        if (waiter == nullptr) {
            continue;
        }

        bool active = true;
        if (!waiter->active.compare_exchange_strong(active,
                                                    false,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            continue;
        }

        auto* client = try_acquire();
        if (client == nullptr) {
            waiter->active.store(true, std::memory_order_release);
            if (!enqueue_waiter(waiter)) {
                waiter->active.store(false, std::memory_order_release);
                waiter->waker.wake_up();
            }
            return false;
        }

        waiter->client.store(client, std::memory_order_release);
        waiter->waker.wake_up();
        return true;
    }
    return false;
}

bool PostgresConnectionPool::fail_one_waiter()
{
    std::shared_ptr<detail::PostgresPoolWaiter> waiter;
    while (m_waiters.try_dequeue(waiter)) {
        if (waiter == nullptr) {
            continue;
        }
        bool active = true;
        if (!waiter->active.compare_exchange_strong(active,
                                                    false,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            continue;
        }
        waiter->client.store(nullptr, std::memory_order_release);
        waiter->waker.wake_up();
        return true;
    }
    return false;
}

void PostgresConnectionPool::release(AsyncPostgresClient<>* client)
{
    if (client == nullptr) {
        return;
    }

    if (client->is_closed() || client->transaction_status() != 'I') {
        recycle_disconnected(client);
        (void)fail_one_waiter();
        return;
    }

    if (!m_idle_clients.enqueue(client)) {
        recycle_disconnected(client);
        (void)fail_one_waiter();
        return;
    }
    m_idle_connections.fetch_add(1, std::memory_order_acq_rel);
    (void)wake_one_waiter();
}

PostgresConnectionPool::AcquireAwaitable PostgresConnectionPool::acquire()
{
    return AcquireAwaitable(*this);
}

PostgresConnectionPool::LeaseAwaitable PostgresConnectionPool::lease()
{
    return LeaseAwaitable(*this);
}

#include "../details/pool_awaitable.inl"

} // namespace galay::postgres
