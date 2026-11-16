/**
 * @file pool_awaitable.h
 * @brief MySQL 连接池对公共协程等待体状态机的协议适配。
 */

#ifndef GALAY_MYSQL_DETAILS_POOL_AWAITABLE_H
#define GALAY_MYSQL_DETAILS_POOL_AWAITABLE_H

#include "../async/conn_pool.h"

namespace galay::mysql::detail
{

/** @brief 为公共连接池 awaitable 提供 MySQL 类型、错误和访问入口。 */
struct MysqlPoolAwaitableTraits
{
    using Pool = MysqlConnectionPool;
    using Client = AsyncMysqlClient<>;
    using Waiter = MysqlPoolWaiter;
    using ConnectAwaitable = MysqlConnectAwaitable<>;
    using Lease = MysqlPoolLease;
    using Error = MysqlError;
    using TaskRef = galay::kernel::TaskRef;

    /**
     * @brief 创建绑定当前协程唤醒器的 MySQL 等待节点。
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

    static const MysqlConfig& connect_config(Pool& pool) noexcept
    {
        return pool.m_mysql_config;
    }

    static Client* waiter_client(Waiter& waiter) noexcept
    {
        return waiter.client.load(std::memory_order_acquire);
    }

    /** @brief MySQL 直接复用失败建连的客户端槽位，无需额外回收到队列。 */
    static void on_connect_failure(Pool&, Client*) noexcept {}

    /**
     * @brief 把公共错误分类转换为 MySQL 错误。
     * @param code 公共等待体错误分类。
     * @return 带 MySQL 内部错误码的协议错误。
     */
    static Error make_error(
        galay::utils::detail::ConnectionPoolAwaitableErrorCode code)
    {
        using ErrorCode =
            galay::utils::detail::ConnectionPoolAwaitableErrorCode;
        switch (code) {
        case ErrorCode::kMissingConnectAwaitable:
            return MysqlError(
                MYSQL_ERROR_INTERNAL,
                "Missing connect awaitable in creating state");
        case ErrorCode::kConnectWithoutValue:
            return MysqlError(
                MYSQL_ERROR_INTERNAL,
                "Connect awaitable resumed without value");
        case ErrorCode::kWaitingWithoutClient:
            return MysqlError(
                MYSQL_ERROR_INTERNAL,
                "Failed to acquire connection after wakeup");
        case ErrorCode::kEnqueueFailed:
            return MysqlError(
                MYSQL_ERROR_INTERNAL,
                "Failed to enqueue connection waiter");
        case ErrorCode::kInvalidState:
            return MysqlError(MYSQL_ERROR_INTERNAL, "Invalid acquire state");
        }
        return MysqlError(MYSQL_ERROR_INTERNAL, "Invalid acquire state");
    }

    static Lease make_lease(Pool& pool, Client* client) noexcept
    {
        return Lease(&pool, client);
    }
};

} // namespace galay::mysql::detail

namespace galay::mysql
{

/** @brief MySQL 连接池的连接获取等待体。 */
class MysqlConnectionPool::AcquireAwaitable
    : public galay::utils::detail::ConnectionPoolAcquireAwaitable<
          detail::MysqlPoolAwaitableTraits>
{
public:
    using Base = galay::utils::detail::ConnectionPoolAcquireAwaitable<
        detail::MysqlPoolAwaitableTraits>;
    using Base::Base;
};

/** @brief MySQL 连接池的 RAII 租约获取等待体。 */
class MysqlConnectionPool::LeaseAwaitable
    : public galay::utils::detail::ConnectionPoolLeaseAwaitable<
          detail::MysqlPoolAwaitableTraits>
{
public:
    using Base = galay::utils::detail::ConnectionPoolLeaseAwaitable<
        detail::MysqlPoolAwaitableTraits>;
    using Base::Base;
};

} // namespace galay::mysql

#endif // GALAY_MYSQL_DETAILS_POOL_AWAITABLE_H
