#ifndef GALAY_MYSQL_DETAILS_POOL_AWAITABLE_H
#define GALAY_MYSQL_DETAILS_POOL_AWAITABLE_H

#include "../async/conn_pool.h"

namespace galay::mysql
{

/**
 * @brief MySQL 连接池连接获取等待体
 * @note 挂起时只注册 Waker 或等待异步连接，不阻塞调用线程。
 */
class MysqlConnectionPool::AcquireAwaitable
{
public:
        /**
         * @brief 构造获取连接等待体
         * @param pool 连接池引用
         */
        AcquireAwaitable(MysqlConnectionPool& pool);

        bool await_ready() const noexcept; ///< 检查是否已完成
        /**
         * @brief 挂起协程，等待连接获取
         * @tparam Promise 协程Promise类型
         * @param handle 协程句柄
         * @return 是否需要挂起
         */
        template <typename Promise>
        requires requires(const Promise& promise) {
            { promise.taskRefView() } -> std::same_as<const galay::kernel::TaskRef&>;
        }
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            if (m_state != State::Invalid) {
                return false;
            }

            m_client = m_pool.tryAcquire();
            if (m_client) {
                m_state = State::Ready;
                m_connect_awaitable.reset();
                return false;
            }

            m_client = m_pool.createClient();
            if (m_client) {
                m_state = State::Creating;
                m_connect_awaitable.emplace(*m_client, m_pool.m_mysql_config);
                return m_connect_awaitable->await_suspend(handle);
            }

            m_state = State::Waiting;
            m_connect_awaitable.reset();
            m_waiter = std::make_shared<galay::mysql::detail::MysqlPoolWaiter>(galay::kernel::Waker(handle));
            if (!m_pool.enqueueWaiter(m_waiter)) {
                m_state = State::EnqueueFailed;
                return false;
            }
            if (m_pool.m_idle_connections.load(std::memory_order_acquire) > 0) {
                (void)m_pool.wakeOneWaiter();
            }
            return true;
        }
        /**
         * @brief 获取连接获取结果
         * @return 成功时返回客户端指针，失败时返回错误
         */
        std::expected<std::optional<AsyncMysqlClient<>*>, MysqlError> await_resume();

    private:
        /**
         * @brief 等待体状态枚举
         */
        enum class State {
            Invalid,  ///< 无效状态
            Ready,    ///< 有空闲连接
            Waiting,  ///< 等待连接释放
            Creating, ///< 正在创建新连接
            EnqueueFailed, ///< 等待队列入队失败
        };

        MysqlConnectionPool& m_pool;                                ///< 连接池引用
        AsyncMysqlClient<>* m_client = nullptr;                        ///< 客户端指针
        std::shared_ptr<galay::mysql::detail::MysqlPoolWaiter> m_waiter;           ///< 等待队列节点
        std::optional<MysqlConnectAwaitable<>> m_connect_awaitable;    ///< 连接等待体
        State m_state = State::Invalid;                              ///< 当前状态
};

/**
 * @brief MySQL 连接池 RAII 租约获取等待体
 * @note 成功恢复后返回 move-only MysqlPoolLease，错误通过 MysqlError 传播。
 */
class MysqlConnectionPool::LeaseAwaitable
{
public:
        explicit LeaseAwaitable(MysqlConnectionPool& pool);

        bool await_ready() const noexcept;
        template <typename Promise>
        requires requires(const Promise& promise) {
            { promise.taskRefView() } -> std::same_as<const galay::kernel::TaskRef&>;
        }
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return m_acquire.await_suspend(handle);
        }
        std::expected<std::optional<MysqlPoolLease>, MysqlError> await_resume();

    private:
        MysqlConnectionPool& m_pool; ///< 生成租约的连接池
        AcquireAwaitable m_acquire;  ///< 底层连接获取等待体
};

} // namespace galay::mysql

#endif // GALAY_MYSQL_DETAILS_POOL_AWAITABLE_H
