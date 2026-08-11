#ifndef GALAY_POSTGRES_ASYNC_CONN_POOL_H
#define GALAY_POSTGRES_ASYNC_CONN_POOL_H

#include "client.h"
#include "../../galay-kernel/core/waker.h"

#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include <atomic>
#include <coroutine>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace galay::postgres
{

class PostgresConnectionPool;

namespace detail
{

struct PostgresPoolWaiter
{
    explicit PostgresPoolWaiter(galay::kernel::Waker waiter_waker)
        : waker(std::move(waiter_waker))
    {
    }

    galay::kernel::Waker waker;
    std::atomic<AsyncPostgresClient<>*> client{nullptr};
    std::atomic<bool> active{true};
};

} // namespace detail

struct PostgresConnectionPoolConfig
{
    PostgresConfig postgres_config = PostgresConfig::defaultConfig();
    AsyncPostgresConfig async_config = AsyncPostgresConfig::noTimeout();
    size_t min_connections = 2;
    size_t max_connections = 10;
};

/**
 * @brief Move-only RAII lease for one exclusively borrowed pool connection.
 * @note The lease must not outlive its pool. Destruction does not perform I/O.
 */
class PostgresPoolLease
{
public:
    PostgresPoolLease() noexcept = default;
    PostgresPoolLease(PostgresPoolLease&& other) noexcept;
    PostgresPoolLease& operator=(PostgresPoolLease&& other) noexcept;
    PostgresPoolLease(const PostgresPoolLease&) = delete;
    PostgresPoolLease& operator=(const PostgresPoolLease&) = delete;
    ~PostgresPoolLease();

    [[nodiscard]] AsyncPostgresClient<>* get() const noexcept { return m_client; }
    AsyncPostgresClient<>& operator*() const noexcept { return *m_client; }
    AsyncPostgresClient<>* operator->() const noexcept { return m_client; }
    explicit operator bool() const noexcept { return m_client != nullptr; }
    void release() noexcept;
    AsyncPostgresClient<>* dismiss() noexcept;

private:
    friend class PostgresConnectionPool;
    PostgresPoolLease(PostgresConnectionPool* pool, AsyncPostgresClient<>* client) noexcept;

    PostgresConnectionPool* m_pool = nullptr;
    AsyncPostgresClient<>* m_client = nullptr;
};

class PostgresConnectionPool
{
public:
    PostgresConnectionPool(galay::kernel::IOScheduler* scheduler,
                           PostgresConnectionPoolConfig config = {});
    ~PostgresConnectionPool();

    PostgresConnectionPool(const PostgresConnectionPool&) = delete;
    PostgresConnectionPool& operator=(const PostgresConnectionPool&) = delete;

    class AcquireAwaitable;
    class LeaseAwaitable;

    AcquireAwaitable acquire();
    LeaseAwaitable lease();
    void release(AsyncPostgresClient<>* client);

    [[nodiscard]] size_t size() const noexcept
    {
        return m_total_connections.load(std::memory_order_acquire);
    }
    [[nodiscard]] size_t idleCount() const noexcept
    {
        return m_idle_connections.load(std::memory_order_acquire);
    }

private:
    friend class AcquireAwaitable;

    AsyncPostgresClient<>* tryAcquire();
    AsyncPostgresClient<>* createClient();
    void recycleDisconnected(AsyncPostgresClient<>* client);
    bool enqueueWaiter(std::shared_ptr<detail::PostgresPoolWaiter> waiter);
    bool wakeOneWaiter();
    bool failOneWaiter();

    galay::kernel::IOScheduler* m_scheduler = nullptr;
    PostgresConfig m_postgres_config;
    AsyncPostgresConfig m_async_config;
    size_t m_min_connections = 0;
    size_t m_max_connections = 0;

    moodycamel::ConcurrentQueue<AsyncPostgresClient<>*> m_idle_clients;
    moodycamel::ConcurrentQueue<AsyncPostgresClient<>*> m_disconnected_clients;
    moodycamel::ConcurrentQueue<std::shared_ptr<detail::PostgresPoolWaiter>> m_waiters;
    std::vector<std::unique_ptr<AsyncPostgresClient<>>> m_all_clients;
    std::atomic<size_t> m_total_connections{0};
    std::atomic<size_t> m_idle_connections{0};
};

} // namespace galay::postgres

#include "../details/pool_awaitable.h"

#endif // GALAY_POSTGRES_ASYNC_CONN_POOL_H
