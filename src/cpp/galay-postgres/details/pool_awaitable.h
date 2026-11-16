/**
 * @file pool_awaitable.h
 * @brief Postgres 连接池对公共协程等待体状态机的协议适配。
 */

#ifndef GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_H
#define GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_H

#include "../async/conn_pool.h"

namespace galay::postgres::detail
{

/** @brief 为公共连接池 awaitable 提供 Postgres 类型、错误和访问入口。 */
struct PostgresPoolAwaitableTraits
{
    using Pool = PostgresConnectionPool;
    using Client = AsyncPostgresClient<>;
    using Waiter = PostgresPoolWaiter;
    using ConnectAwaitable = PostgresConnectAwaitable<>;
    using Lease = PostgresPoolLease;
    using Error = PostgresError;
    using TaskRef = galay::kernel::TaskRef;

    /**
     * @brief 创建绑定当前协程唤醒器的 Postgres 等待节点。
     * @tparam Promise 协程 promise 类型。
     * @param handle 当前协程句柄。
     * @return 连接池等待节点。
     */
    template <typename Promise>
    static std::shared_ptr<Waiter> make_waiter(
        std::coroutine_handle<Promise> handle)
    {
        return std::make_shared<Waiter>(galay::kernel::Waker(handle));
    }

    static Client* try_acquire(Pool& pool) { return pool.try_acquire(); }
    static Client* create_client(Pool& pool) { return pool.create_client(); }

    static bool enqueue_waiter(Pool& pool,
                               const std::shared_ptr<Waiter>& waiter)
    {
        return pool.enqueue_waiter(waiter);
    }

    static bool has_idle_connection(Pool& pool) noexcept
    {
        return pool.m_idle_connections.load(std::memory_order_acquire) > 0;
    }

    static bool wake_one_waiter(Pool& pool) { return pool.wake_one_waiter(); }

    static const PostgresConfig& connect_config(Pool& pool) noexcept
    {
        return pool.m_postgres_config;
    }

    static Client* waiter_client(Waiter& waiter) noexcept
    {
        return waiter.client.load(std::memory_order_acquire);
    }

    /** @brief Postgres 建连失败时把客户端槽位放回可重建队列。 */
    static void on_connect_failure(Pool& pool, Client* client)
    {
        pool.recycle_disconnected(client);
    }

    /**
     * @brief 把公共错误分类转换为 Postgres 错误。
     * @param code 公共等待体错误分类。
     * @return 带 Postgres 内部错误码的协议错误。
     */
    static Error make_error(
        galay::utils::detail::ConnectionPoolAwaitableErrorCode code)
    {
        using ErrorCode =
            galay::utils::detail::ConnectionPoolAwaitableErrorCode;
        switch (code) {
        case ErrorCode::kMissingConnectAwaitable:
            return PostgresError(
                POSTGRES_ERROR_INTERNAL,
                "Missing connect awaitable while creating a pool connection");
        case ErrorCode::kConnectWithoutValue:
            return PostgresError(
                POSTGRES_ERROR_INTERNAL,
                "Connect awaitable resumed without a final value");
        case ErrorCode::kWaitingWithoutClient:
            return PostgresError(
                POSTGRES_ERROR_INTERNAL,
                "Pool waiter resumed without a connection");
        case ErrorCode::kEnqueueFailed:
            return PostgresError(
                POSTGRES_ERROR_INTERNAL,
                "Failed to enqueue PostgreSQL pool waiter");
        case ErrorCode::kInvalidState:
            return PostgresError(POSTGRES_ERROR_INTERNAL,
                                 "Invalid PostgreSQL pool acquire state");
        }
        return PostgresError(POSTGRES_ERROR_INTERNAL,
                             "Invalid PostgreSQL pool acquire state");
    }

    static Lease make_lease(Pool& pool, Client* client) noexcept
    {
        return Lease(&pool, client);
    }
};

} // namespace galay::postgres::detail

namespace galay::postgres
{

/** @brief Postgres 连接池的连接获取等待体。 */
class PostgresConnectionPool::AcquireAwaitable
    : public galay::utils::detail::ConnectionPoolAcquireAwaitable<
          detail::PostgresPoolAwaitableTraits>
{
public:
    using Base = galay::utils::detail::ConnectionPoolAcquireAwaitable<
        detail::PostgresPoolAwaitableTraits>;
    using Base::Base;
};

/** @brief Postgres 连接池的 RAII 租约获取等待体。 */
class PostgresConnectionPool::LeaseAwaitable
    : public galay::utils::detail::ConnectionPoolLeaseAwaitable<
          detail::PostgresPoolAwaitableTraits>
{
public:
    using Base = galay::utils::detail::ConnectionPoolLeaseAwaitable<
        detail::PostgresPoolAwaitableTraits>;
    using Base::Base;
};

} // namespace galay::postgres

#endif // GALAY_POSTGRES_DETAILS_POOL_AWAITABLE_H
