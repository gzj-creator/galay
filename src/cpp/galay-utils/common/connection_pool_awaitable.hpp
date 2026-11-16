/**
 * @file connection_pool_awaitable.hpp
 * @brief 数据库连接池共用的协程获取与租约等待体状态机。
 */

#ifndef GALAY_UTILS_COMMON_CONNECTION_POOL_AWAITABLE_HPP
#define GALAY_UTILS_COMMON_CONNECTION_POOL_AWAITABLE_HPP

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

namespace galay::utils::detail
{

/** @brief 连接池等待体在公共状态机中的错误分类。 */
enum class ConnectionPoolAwaitableErrorCode : uint8_t
{
    kMissingConnectAwaitable, ///< 创建状态缺少底层建连等待体。
    kConnectWithoutValue,     ///< 建连等待体成功恢复但没有返回值。
    kWaitingWithoutClient,    ///< waiter 恢复后没有取得连接。
    kEnqueueFailed,           ///< waiter 无法进入连接池等待队列。
    kInvalidState,            ///< await_resume 观察到非法状态。
};

/**
 * @brief 校验协程 promise 能提供连接池等待体要求的任务引用视图。
 * @tparam Traits 协议侧的连接池 awaitable traits。
 * @tparam Promise 协程 promise 类型。
 */
template <typename Traits, typename Promise>
concept ConnectionPoolAwaitablePromise = requires(const Promise& promise) {
    { promise.task_ref_view() } ->
        std::same_as<const typename Traits::TaskRef&>;
};

/**
 * @brief 连接池获取连接的公共协程状态机。
 * @tparam Traits 提供协议类型、错误转换和连接池访问入口的 traits。
 * @note 挂起时只注册唤醒器或等待异步建连，不阻塞调用线程。
 */
template <typename Traits>
class ConnectionPoolAcquireAwaitable
{
public:
    using Pool = typename Traits::Pool;            ///< 连接池类型。
    using Client = typename Traits::Client;        ///< 连接客户端类型。
    using Waiter = typename Traits::Waiter;        ///< 等待队列节点类型。
    using ConnectAwaitable = typename Traits::ConnectAwaitable; ///< 异步建连等待体类型。
    using Error = typename Traits::Error;          ///< 协议错误类型。
    using Result = std::expected<std::optional<Client*>, Error>; ///< 获取结果类型。

    /**
     * @brief 创建绑定指定连接池的获取等待体。
     * @param pool 连接池引用。
     */
    explicit ConnectionPoolAcquireAwaitable(Pool& pool) noexcept
        : m_pool(pool)
    {
    }

    /**
     * @brief 检查获取操作能否同步完成。
     * @return 固定返回 false，由 await_suspend 决定即时恢复或挂起。
     */
    bool await_ready() const noexcept { return false; }

    /**
     * @brief 尝试复用、创建或排队等待连接。
     * @tparam Promise 协程 promise 类型。
     * @param handle 当前协程句柄。
     * @return 需要挂起当前协程时返回 true。
     */
    template <typename Promise>
    requires ConnectionPoolAwaitablePromise<Traits, Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        if (m_state != State::kInvalid) {
            return false;
        }

        m_client = Traits::try_acquire(m_pool);
        if (m_client != nullptr) {
            m_state = State::kReady;
            m_connect_awaitable.reset();
            return false;
        }

        m_client = Traits::create_client(m_pool);
        if (m_client != nullptr) {
            m_state = State::kCreating;
            m_connect_awaitable.emplace(
                *m_client, Traits::connect_config(m_pool));
            return m_connect_awaitable->await_suspend(handle);
        }

        m_state = State::kWaiting;
        m_connect_awaitable.reset();
        m_waiter = Traits::template make_waiter<Promise>(handle);
        if (!Traits::enqueue_waiter(m_pool, m_waiter)) {
            m_state = State::kEnqueueFailed;
            return false;
        }
        if (Traits::has_idle_connection(m_pool)) {
            (void)Traits::wake_one_waiter(m_pool);
        }
        return true;
    }

    /**
     * @brief 取得连接、建连错误或排队等待结果。
     * @return 成功时持有连接指针，失败时返回协议错误。
     */
    Result await_resume()
    {
        if (m_state == State::kReady) {
            Client* client = finish_operation();
            return std::optional<Client*>{client};
        }

        if (m_state == State::kCreating) {
            if (!m_connect_awaitable.has_value()) {
                Client* client = finish_operation();
                Traits::on_connect_failure(m_pool, client);
                return std::unexpected(Traits::make_error(
                    ConnectionPoolAwaitableErrorCode::kMissingConnectAwaitable));
            }

            auto connected = m_connect_awaitable->await_resume();
            m_connect_awaitable.reset();
            if (!connected) {
                Client* client = finish_operation();
                Traits::on_connect_failure(m_pool, client);
                return std::unexpected(connected.error());
            }
            if (!connected->has_value()) {
                Client* client = finish_operation();
                Traits::on_connect_failure(m_pool, client);
                return std::unexpected(Traits::make_error(
                    ConnectionPoolAwaitableErrorCode::kConnectWithoutValue));
            }

            Client* client = finish_operation();
            return std::optional<Client*>{client};
        }

        if (m_state == State::kWaiting) {
            m_client = m_waiter == nullptr
                ? nullptr
                : Traits::waiter_client(*m_waiter);
            Client* client = finish_operation();
            if (client != nullptr) {
                return std::optional<Client*>{client};
            }
            return std::unexpected(Traits::make_error(
                ConnectionPoolAwaitableErrorCode::kWaitingWithoutClient));
        }

        if (m_state == State::kEnqueueFailed) {
            reset_operation();
            return std::unexpected(Traits::make_error(
                ConnectionPoolAwaitableErrorCode::kEnqueueFailed));
        }

        reset_operation();
        return std::unexpected(Traits::make_error(
            ConnectionPoolAwaitableErrorCode::kInvalidState));
    }

private:
    enum class State : uint8_t
    {
        kInvalid,
        kReady,
        kWaiting,
        kCreating,
        kEnqueueFailed,
    };

    /**
     * @brief 结束当前操作并交还连接所有权。
     * @return 当前操作持有的连接，没有连接时返回 nullptr。
     */
    Client* finish_operation() noexcept
    {
        m_state = State::kInvalid;
        m_waiter.reset();
        m_connect_awaitable.reset();
        return std::exchange(m_client, nullptr);
    }

    /** @brief 清除当前操作的全部中间状态。 */
    void reset_operation() noexcept
    {
        m_state = State::kInvalid;
        m_client = nullptr;
        m_waiter.reset();
        m_connect_awaitable.reset();
    }

    Pool& m_pool; ///< 目标连接池。
    Client* m_client = nullptr; ///< 当前连接或正在建立连接的客户端。
    std::shared_ptr<Waiter> m_waiter; ///< 排队等待节点。
    std::optional<ConnectAwaitable> m_connect_awaitable; ///< 异步建连等待体。
    State m_state = State::kInvalid; ///< 当前获取阶段。
};

/**
 * @brief 把成功获取的裸连接包装为协议 RAII 租约。
 * @tparam Traits 提供协议类型和租约构造入口的 traits。
 */
template <typename Traits>
class ConnectionPoolLeaseAwaitable
{
public:
    using Pool = typename Traits::Pool;   ///< 连接池类型。
    using Lease = typename Traits::Lease; ///< 协议租约类型。
    using Error = typename Traits::Error; ///< 协议错误类型。
    using Result = std::expected<std::optional<Lease>, Error>; ///< 租约获取结果。

    /**
     * @brief 创建绑定指定连接池的租约等待体。
     * @param pool 连接池引用。
     */
    explicit ConnectionPoolLeaseAwaitable(Pool& pool) noexcept
        : m_pool(pool)
        , m_acquire(pool)
    {
    }

    /**
     * @brief 检查租约获取能否同步完成。
     * @return 底层获取等待体的检查结果。
     */
    bool await_ready() const noexcept { return m_acquire.await_ready(); }

    /**
     * @brief 把协程挂起委托给底层连接获取状态机。
     * @tparam Promise 协程 promise 类型。
     * @param handle 当前协程句柄。
     * @return 需要挂起当前协程时返回 true。
     */
    template <typename Promise>
    requires ConnectionPoolAwaitablePromise<Traits, Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_acquire.await_suspend(handle);
    }

    /**
     * @brief 将底层连接结果转换为 RAII 租约。
     * @return 成功时持有协议租约，失败时返回协议错误。
     */
    Result await_resume()
    {
        auto acquired = m_acquire.await_resume();
        if (!acquired) {
            return std::unexpected(acquired.error());
        }
        if (!acquired->has_value()) {
            return std::optional<Lease>{};
        }
        std::optional<Lease> lease;
        lease.emplace(Traits::make_lease(m_pool, acquired->value()));
        return lease;
    }

private:
    Pool& m_pool; ///< 生成租约的连接池。
    ConnectionPoolAcquireAwaitable<Traits> m_acquire; ///< 底层连接获取等待体。
};

} // namespace galay::utils::detail

#endif // GALAY_UTILS_COMMON_CONNECTION_POOL_AWAITABLE_HPP
