#ifndef GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_H
#define GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_H

#include "../async/conn_pool.h"

#include <concepts>

namespace galay::postgres
{

class PostgresConnectionPool::AcquireAwaitable
{
public:
    explicit AcquireAwaitable(PostgresConnectionPool& pool);

    bool await_ready() const noexcept { return false; }

    template<typename Promise>
    requires requires(const Promise& promise) {
        { promise.taskRefView() } -> std::same_as<const galay::kernel::TaskRef&>;
    }
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        if (m_state != State::Invalid) {
            return false;
        }

        m_client = m_pool.tryAcquire();
        if (m_client != nullptr) {
            m_state = State::Ready;
            return false;
        }

        m_client = m_pool.createClient();
        if (m_client != nullptr) {
            m_state = State::Creating;
            m_connect_awaitable.emplace(*m_client, m_pool.m_postgres_config);
            return m_connect_awaitable->await_suspend(handle);
        }

        m_state = State::Waiting;
        m_waiter = std::make_shared<detail::PostgresPoolWaiter>(galay::kernel::Waker(handle));
        if (!m_pool.enqueueWaiter(m_waiter)) {
            m_state = State::EnqueueFailed;
            return false;
        }
        if (m_pool.m_idle_connections.load(std::memory_order_acquire) > 0) {
            (void)m_pool.wakeOneWaiter();
        }
        return true;
    }

    std::expected<std::optional<AsyncPostgresClient<>*>, PostgresError> await_resume();

private:
    enum class State
    {
        Invalid,
        Ready,
        Waiting,
        Creating,
        EnqueueFailed,
    };

    PostgresConnectionPool& m_pool;
    AsyncPostgresClient<>* m_client = nullptr;
    std::shared_ptr<detail::PostgresPoolWaiter> m_waiter;
    std::optional<PostgresConnectAwaitable<>> m_connect_awaitable;
    State m_state = State::Invalid;
};

class PostgresConnectionPool::LeaseAwaitable
{
public:
    explicit LeaseAwaitable(PostgresConnectionPool& pool);

    bool await_ready() const noexcept { return m_acquire.await_ready(); }

    template<typename Promise>
    requires requires(const Promise& promise) {
        { promise.taskRefView() } -> std::same_as<const galay::kernel::TaskRef&>;
    }
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_acquire.await_suspend(handle);
    }

    std::expected<std::optional<PostgresPoolLease>, PostgresError> await_resume();

private:
    PostgresConnectionPool& m_pool;
    AcquireAwaitable m_acquire;
};

} // namespace galay::postgres

#endif // GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_H
