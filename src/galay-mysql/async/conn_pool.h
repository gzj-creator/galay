/**
 * @file conn_pool.h
 * @brief 异步MySQL连接池
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 提供异步MySQL连接池管理，支持连接获取、归还、自动创建和等待。
 *          使用AcquireAwaitable实现协程友好的连接获取。
 */

#ifndef GALAY_MYSQL_CONNECTION_POOL_H
#define GALAY_MYSQL_CONNECTION_POOL_H

#include "galay-mysql/base/mysql_config.h"
#include "client.h"
#include <galay-kernel/core/io_scheduler.hpp>
#include <galay-kernel/core/task.h>
#include <galay-kernel/concurrency/async_waiter.h>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <optional>
#include <coroutine>

namespace galay::mysql
{

/**
 * @brief MySQL连接池配置
 * @details 定义连接池的基本参数，包括MySQL连接配置、异步配置以及连接数限制。
 */
struct MysqlConnectionPoolConfig
{
    MysqlConfig mysql_config = MysqlConfig::defaultConfig();   ///< MySQL连接配置
    AsyncMysqlConfig async_config = AsyncMysqlConfig::noTimeout(); ///< 异步配置
    size_t min_connections = 2;  ///< 最小连接数
    size_t max_connections = 10; ///< 最大连接数
};

/**
 * @brief 异步MySQL连接池
 * @details 管理多个AsyncMysqlClient连接，支持异步获取和归还。
 *          当空闲连接不足时自动创建新连接，达到上限后等待其他协程归还连接。
 */
class MysqlConnectionPool
{
public:
    /**
     * @brief 构造连接池
     * @param scheduler IO调度器指针
     * @param config 连接池配置
     */
    MysqlConnectionPool(galay::kernel::IOScheduler* scheduler,
                        MysqlConnectionPoolConfig config = {});

    ~MysqlConnectionPool(); ///< 析构连接池

    MysqlConnectionPool(const MysqlConnectionPool&) = delete;             ///< 禁止拷贝构造
    MysqlConnectionPool& operator=(const MysqlConnectionPool&) = delete;  ///< 禁止拷贝赋值

    /**
     * @brief 获取连接的Awaitable
     * @details 如果池中有空闲连接则立即返回，否则创建新连接或等待
     */
    class AcquireAwaitable
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
            std::lock_guard<std::mutex> lock(m_pool.m_mutex);
            m_pool.m_waiters.push(handle);
            return true;
        }
        /**
         * @brief 获取连接获取结果
         * @return 成功时返回客户端指针，失败时返回错误
         */
        std::expected<std::optional<AsyncMysqlClient*>, MysqlError> await_resume();

    private:
        /**
         * @brief 等待体状态枚举
         */
        enum class State {
            Invalid,  ///< 无效状态
            Ready,    ///< 有空闲连接
            Waiting,  ///< 等待连接释放
            Creating, ///< 正在创建新连接
        };

        MysqlConnectionPool& m_pool;                                ///< 连接池引用
        State m_state;                                               ///< 当前状态
        AsyncMysqlClient* m_client = nullptr;                        ///< 客户端指针
        std::optional<MysqlConnectAwaitable> m_connect_awaitable;    ///< 连接等待体
    };

    /**
     * @brief 获取一个连接
     * @return 连接获取等待体
     */
    AcquireAwaitable acquire();

    /**
     * @brief 归还连接到池中
     * @param client 要归还的客户端指针
     */
    void release(AsyncMysqlClient* client);

    /**
     * @brief 获取当前池中连接数
     * @return 连接总数
     */
    size_t size() const { return m_total_connections.load(std::memory_order_relaxed); }

    /**
     * @brief 获取空闲连接数
     * @return 空闲连接数
     */
    size_t idleCount() const;

private:
    friend class AcquireAwaitable;

    AsyncMysqlClient* tryAcquire();  ///< 尝试从空闲队列获取连接
    AsyncMysqlClient* createClient(); ///< 创建新的客户端连接

    galay::kernel::IOScheduler* m_scheduler;        ///< IO调度器指针
    MysqlConfig m_mysql_config;                      ///< MySQL连接配置
    AsyncMysqlConfig m_async_config;                 ///< 异步配置
    size_t m_min_connections;                        ///< 最小连接数
    size_t m_max_connections;                        ///< 最大连接数

    mutable std::mutex m_mutex;                              ///< 互斥锁
    std::queue<AsyncMysqlClient*> m_idle_clients;            ///< 空闲客户端队列
    std::vector<std::unique_ptr<AsyncMysqlClient>> m_all_clients; ///< 所有客户端
    std::queue<std::coroutine_handle<>> m_waiters;           ///< 等待者队列
    std::atomic<size_t> m_total_connections{0};              ///< 总连接数

};

} // namespace galay::mysql

#endif // GALAY_MYSQL_CONNECTION_POOL_H
