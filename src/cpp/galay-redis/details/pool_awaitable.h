#ifndef GALAY_REDIS_DETAILS_POOL_AWAITABLE_H
#define GALAY_REDIS_DETAILS_POOL_AWAITABLE_H

#include "../async/conn_pool.h"

namespace galay::redis
{

    /**
     * @brief 连接池初始化等待体
     */
    class PoolInitializeAwaitable : public galay::kernel::TimeoutSupport<PoolInitializeAwaitable>
    {
    public:
        using Result = RedisVoidResult;
        static constexpr bool kAlwaysReady = true;

        PoolInitializeAwaitable(RedisConnectionPool& pool);
        PoolInitializeAwaitable(const PoolInitializeAwaitable&) = delete;
        PoolInitializeAwaitable& operator=(const PoolInitializeAwaitable&) = delete;
        PoolInitializeAwaitable(PoolInitializeAwaitable&&) noexcept = default;
        PoolInitializeAwaitable& operator=(PoolInitializeAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return true; }
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise>)
        {
            return false;
        }
        Result await_resume() { return std::move(m_result); }

    private:
        Result m_result;
    };

    /**
     * @brief 连接池获取连接等待体
     */
    class PoolAcquireAwaitable : public galay::kernel::TimeoutSupport<PoolAcquireAwaitable>
    {
    public:
        using Result = std::expected<std::shared_ptr<PooledConnection>, RedisError>;

        PoolAcquireAwaitable(RedisConnectionPool& pool);
        PoolAcquireAwaitable(const PoolAcquireAwaitable&) = delete;
        PoolAcquireAwaitable& operator=(const PoolAcquireAwaitable&) = delete;
        PoolAcquireAwaitable(PoolAcquireAwaitable&&) noexcept = default;
        PoolAcquireAwaitable& operator=(PoolAcquireAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return false; }
        template <typename Promise>
        requires requires(const Promise& promise) {
            { promise.task_ref_view() } -> std::same_as<const galay::kernel::TaskRef&>;
        }
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            const auto action = prepare_suspend(galay::kernel::Waker(handle));
            if (action == SuspendAction::Wait) {
                return true;
            }
            if (action == SuspendAction::Connect && m_connect_awaitable.has_value()) {
                return m_connect_awaitable->await_suspend(handle);
            }
            return false;
        }
        Result await_resume();
        void mark_timeout();

    private:
        enum class SuspendAction {
            Ready,
            Connect,
            Wait,
            Error,
        };

        enum class State {
            Invalid,
            Ready,
            Creating,
            Waiting,
            EnqueueFailed,
            TimedOut,
            Error,
        };

        SuspendAction prepare_suspend(galay::kernel::Waker waiter_waker);

        std::optional<RedisConnectOperation> m_connect_awaitable;
        std::optional<RedisError> m_error;
        std::shared_ptr<PooledConnection> m_connection;
        std::shared_ptr<detail::RedisPoolWaiter> m_waiter;
        RedisConnectionPool* m_pool = nullptr;
        std::chrono::steady_clock::time_point m_start_time;
        State m_state = State::Invalid;
        bool m_wait_counted = false;
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss 连接池初始化等待体
     * @details 用于协程中等待 Rediss 连接池初始化完成
     */
    class RedissPoolInitializeAwaitable
        : public galay::kernel::TimeoutSupport<RedissPoolInitializeAwaitable>
    {
    public:
        using Result = RedisVoidResult;
        static constexpr bool kAlwaysReady = true;

        explicit RedissPoolInitializeAwaitable(RedissConnectionPool& pool);
        RedissPoolInitializeAwaitable(const RedissPoolInitializeAwaitable&) = delete;
        RedissPoolInitializeAwaitable& operator=(const RedissPoolInitializeAwaitable&) = delete;
        RedissPoolInitializeAwaitable(RedissPoolInitializeAwaitable&&) noexcept = default;
        RedissPoolInitializeAwaitable& operator=(RedissPoolInitializeAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return true; }
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise>)
        {
            return false;
        }
        Result await_resume() { return std::move(m_result); }

    private:
        Result m_result;
    };

    /**
     * @brief Rediss 连接池获取连接等待体
     * @details 用于协程中等待从 Rediss 连接池获取可用连接
     */
    class RedissPoolAcquireAwaitable : public galay::kernel::TimeoutSupport<RedissPoolAcquireAwaitable>
    {
    public:
        using Result = std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>;

        explicit RedissPoolAcquireAwaitable(RedissConnectionPool& pool);
        RedissPoolAcquireAwaitable(const RedissPoolAcquireAwaitable&) = delete;
        RedissPoolAcquireAwaitable& operator=(const RedissPoolAcquireAwaitable&) = delete;
        RedissPoolAcquireAwaitable(RedissPoolAcquireAwaitable&&) noexcept = default;
        RedissPoolAcquireAwaitable& operator=(RedissPoolAcquireAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return false; }
        template <typename Promise>
        requires requires(const Promise& promise) {
            { promise.task_ref_view() } -> std::same_as<const galay::kernel::TaskRef&>;
        }
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            const auto action = prepare_suspend(galay::kernel::Waker(handle));
            if (action == SuspendAction::Wait) {
                return true;
            }
            if (action == SuspendAction::Connect && m_connect_awaitable.has_value()) {
                return m_connect_awaitable->await_suspend(handle);
            }
            return false;
        }
        Result await_resume();
        void mark_timeout();

    private:
        enum class SuspendAction {
            Ready,
            Connect,
            Wait,
            Error,
        };

        enum class State {
            Invalid,
            Ready,
            Creating,
            Waiting,
            EnqueueFailed,
            TimedOut,
            Error,
        };

        SuspendAction prepare_suspend(galay::kernel::Waker waiter_waker);

        std::optional<detail::RedissConnectOperation> m_connect_awaitable;
        std::optional<RedisError> m_error;
        std::shared_ptr<PooledRedissConnection> m_connection;
        std::shared_ptr<detail::RedissPoolWaiter> m_waiter;
        RedissConnectionPool* m_pool = nullptr;
        std::chrono::steady_clock::time_point m_start_time;
        State m_state = State::Invalid;
        bool m_wait_counted = false;
    };
#endif

} // namespace galay::redis

#endif // GALAY_REDIS_DETAILS_POOL_AWAITABLE_H
