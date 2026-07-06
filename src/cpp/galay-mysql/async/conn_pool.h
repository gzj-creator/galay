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

#include "../base/mysql_config.h"
#include "client.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/task.h"
#include "../../galay-kernel/core/waker.h"
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <memory>
#include <vector>
#include <atomic>
#include <optional>
#include <coroutine>
#include <utility>

namespace galay::mysql
{

class MysqlConnectionPool;

namespace detail
{

struct MysqlPoolWaiter {
    explicit MysqlPoolWaiter(galay::kernel::Waker waiter_waker)
        : waker(std::move(waiter_waker))
    {
    }

    galay::kernel::Waker waker;                 ///< 等待协程唤醒器
    std::atomic<AsyncMysqlClient*> client{nullptr}; ///< 唤醒前预留给该等待者的连接
    std::atomic<bool> active{true};             ///< 防止陈旧等待节点重复唤醒
};

} // namespace detail

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
 * @brief MySQL连接池租约
 * @details move-only RAII句柄，拥有一次从连接池借出的连接。析构时会以best-effort方式
 *          将连接归还给原连接池；如需转交裸指针所有权，可调用dismiss()并由调用方负责归还。
 * @note 析构和release()不执行异步清理，不阻塞调用线程。
 */
class MysqlPoolLease
{
public:
    MysqlPoolLease() noexcept; ///< 构造空租约
    MysqlPoolLease(MysqlPoolLease&& other) noexcept; ///< 移动构造，源租约失效
    MysqlPoolLease& operator=(MysqlPoolLease&& other) noexcept; ///< 移动赋值，先归还当前连接
    MysqlPoolLease(const MysqlPoolLease&) = delete; ///< 禁止拷贝
    MysqlPoolLease& operator=(const MysqlPoolLease&) = delete; ///< 禁止拷贝赋值
    ~MysqlPoolLease(); ///< 析构时归还仍持有的连接

    AsyncMysqlClient* get() const noexcept; ///< 获取底层连接指针
    AsyncMysqlClient& operator*() const noexcept; ///< 解引用底层连接
    AsyncMysqlClient* operator->() const noexcept; ///< 访问底层连接
    explicit operator bool() const noexcept; ///< 是否持有连接
    void release() noexcept; ///< 立即归还连接；可重复调用
    AsyncMysqlClient* dismiss() noexcept; ///< 放弃RAII归还责任并返回底层连接

private:
    friend class MysqlConnectionPool;

    MysqlPoolLease(MysqlConnectionPool* pool, AsyncMysqlClient* client) noexcept;

    MysqlConnectionPool* m_pool = nullptr; ///< 归还目标连接池
    AsyncMysqlClient* m_client = nullptr;  ///< 当前持有连接
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
     * @details 如果池中有空闲连接则立即返回，否则创建新连接或挂起等待。
     * @note 不阻塞调用线程；调用方需保证连接池在等待体完成前保持存活。
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
            m_waiter = std::make_shared<detail::MysqlPoolWaiter>(galay::kernel::Waker(handle));
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
            EnqueueFailed, ///< 等待队列入队失败
        };

        MysqlConnectionPool& m_pool;                                ///< 连接池引用
        AsyncMysqlClient* m_client = nullptr;                        ///< 客户端指针
        std::shared_ptr<detail::MysqlPoolWaiter> m_waiter;           ///< 等待队列节点
        std::optional<MysqlConnectAwaitable> m_connect_awaitable;    ///< 连接等待体
        State m_state = State::Invalid;                              ///< 当前状态
    };

    /**
     * @brief 获取连接租约的Awaitable
     * @details 包装现有acquire()路径，成功时返回RAII租约，租约析构会归还连接。
     * @note 不阻塞调用线程；调用方需保证连接池在等待体和租约生命周期内保持存活。
     */
    class LeaseAwaitable
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

    /**
     * @brief 获取一个连接
     * @return 连接获取等待体
     */
    AcquireAwaitable acquire();

    /**
     * @brief 获取一个RAII连接租约
     * @return 连接租约获取等待体
     */
    LeaseAwaitable acquireLease();

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
    bool enqueueWaiter(std::shared_ptr<detail::MysqlPoolWaiter> waiter); ///< 注册等待连接的协程
    bool wakeOneWaiter(); ///< 唤醒一个仍然有效的等待协程

    galay::kernel::IOScheduler* m_scheduler;        ///< IO调度器指针
    MysqlConfig m_mysql_config;                      ///< MySQL连接配置
    AsyncMysqlConfig m_async_config;                 ///< 异步配置
    size_t m_min_connections;                        ///< 最小连接数
    size_t m_max_connections;                        ///< 最大连接数

    moodycamel::ConcurrentQueue<AsyncMysqlClient*> m_idle_clients; ///< 空闲客户端队列（无锁）
    moodycamel::ConcurrentQueue<std::shared_ptr<detail::MysqlPoolWaiter>> m_waiters; ///< 等待者队列（无锁）
    std::vector<std::unique_ptr<AsyncMysqlClient>> m_all_clients; ///< 所有客户端槽位，构造后不扩容
    std::atomic<size_t> m_total_connections{0};              ///< 总连接数
    std::atomic<size_t> m_idle_connections{0};               ///< 空闲连接数

};

} // namespace galay::mysql

#endif // GALAY_MYSQL_CONNECTION_POOL_H
