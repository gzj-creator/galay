#ifndef GALAY_REDIS_DETAILS_POOL_AWAITABLE_INL
#define GALAY_REDIS_DETAILS_POOL_AWAITABLE_INL

/**
 * @file pool_awaitable.inl
 * @brief Redis/Rediss 连接池等待体实现
 * @details 仅由 async/conn_pool.cc 在内部计数与错误辅助函数定义后包含。
 */

    // ======================== PoolInitializeAwaitable 实现 ========================

    PoolInitializeAwaitable::PoolInitializeAwaitable(RedisConnectionPool& pool)
        : m_result(pool.initializeSync())
    {
    }

    // ======================== PoolAcquireAwaitable 实现 ========================

    PoolAcquireAwaitable::PoolAcquireAwaitable(RedisConnectionPool& pool)
        : m_pool(&pool)
        , m_start_time(std::chrono::steady_clock::now())
    {
    }

    PoolAcquireAwaitable::SuspendAction
    PoolAcquireAwaitable::prepareSuspend(galay::kernel::Waker waiter_waker)
    {
        m_start_time = std::chrono::steady_clock::now();
        if (m_pool == nullptr) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "Connection pool is missing");
            return SuspendAction::Error;
        }
        if (!m_pool->m_is_initialized) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "Connection pool not initialized");
            return SuspendAction::Error;
        }
        if (m_pool->m_is_shutting_down) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "Connection pool is shutting down");
            return SuspendAction::Error;
        }

        m_connection = m_pool->tryAcquireAvailable();
        if (m_connection) {
            incrementCounter(m_pool->m_total_acquired);
            incrementCounter(m_pool->m_active_connections);
            m_state = State::Ready;
        }

        if (m_state == State::Ready) {
        } else if ((m_connection = m_pool->createConnectionSlot())) {
            m_state = State::Creating;
        } else {
            m_waiter = std::make_shared<detail::RedisPoolWaiter>(std::move(waiter_waker));
            if (m_waiter == nullptr) {
                m_state = State::EnqueueFailed;
                return SuspendAction::Error;
            }
            if (!m_pool->enqueueWaiter(m_waiter)) {
                m_state = State::EnqueueFailed;
                m_waiter.reset();
                return SuspendAction::Error;
            }
            incrementCounter(m_pool->m_waiting_requests);
            m_wait_counted = true;
            m_state = State::Waiting;
            if (m_pool->m_idle_connections.load(std::memory_order_acquire) > 0) {
                const bool woke_waiter = m_pool->wakeOneWaiterFromAvailable();
                if (!woke_waiter && m_pool->m_idle_connections.load(std::memory_order_acquire) > 0) {
                    REDIS_LOG_DEBUG("[client]", "Redis pool waiter queued while idle wake raced");
                }
            }
            return SuspendAction::Wait;
        }

        if (m_state == State::Ready) {
            m_pool->recordAcquireStats(m_start_time);
            return SuspendAction::Ready;
        }
        if (m_state != State::Creating || !m_connection) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "Invalid connection acquire state");
            return SuspendAction::Error;
        }

        RedisConnectOptions options;
        options.username = m_pool->m_config.username;
        options.password = m_pool->m_config.password;
        options.db_index = m_pool->m_config.db_index;
        const bool connect_stored = emplaceOptional(
            m_connect_awaitable,
            m_connection->get()->connect(m_pool->m_config.host, m_pool->m_config.port, std::move(options)));
        if (!connect_stored) {
            m_state = State::Error;
            setAcquireError(m_error,
                            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                            "Failed to store Redis connect awaitable");
            return SuspendAction::Error;
        }
        return SuspendAction::Connect;
    }

    PoolAcquireAwaitable::Result PoolAcquireAwaitable::await_resume()
    {
        if (m_state == State::Ready) {
            m_state = State::Invalid;
            return std::move(m_connection);
        }

        if (m_state == State::Creating) {
            if (!m_connect_awaitable.has_value() || !m_connection) {
                m_state = State::Invalid;
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Missing Redis connect awaitable"));
            }

            auto connect_result = m_connect_awaitable->await_resume();
            m_connect_awaitable.reset();
            if (!connect_result) {
                if (m_pool != nullptr) {
                    m_pool->destroyConnectionSlot(m_connection);
                }
                m_state = State::Invalid;
                m_connection.reset();
                return std::unexpected(connect_result.error());
            }

            m_connection->setHealthy(true);
            m_connection->updateLastUsed();
            if (m_pool != nullptr) {
                incrementCounter(m_pool->m_total_acquired);
                incrementCounter(m_pool->m_active_connections);
                m_pool->recordAcquireStats(m_start_time);
            }
            m_state = State::Invalid;
            return std::move(m_connection);
        }

        if (m_state == State::Waiting) {
            if (m_wait_counted && m_pool != nullptr) {
                decrementCounter(m_pool->m_waiting_requests);
                m_wait_counted = false;
            }

            std::shared_ptr<PooledConnection> connection;
            if (m_pool != nullptr && m_waiter != nullptr) {
                const auto waiter_state = m_waiter->state.load(std::memory_order_acquire);
                if (waiter_state == detail::PoolWaiterState::Completed) {
                    connection = std::move(m_waiter->connection);
                } else if (waiter_state == detail::PoolWaiterState::TimedOut) {
                    m_waiter.reset();
                    m_state = State::Invalid;
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                        "Timed out waiting for Redis pool connection"));
                }
            }
            m_waiter.reset();
            m_state = State::Invalid;
            if (connection) {
                if (m_pool != nullptr) {
                    incrementCounter(m_pool->m_total_acquired);
                    incrementCounter(m_pool->m_active_connections);
                    m_pool->recordAcquireStats(m_start_time);
                }
                m_connection = std::move(connection);
                return std::move(m_connection);
            }
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Failed to acquire connection after wakeup"));
        }

        if (m_state == State::TimedOut) {
            m_state = State::Invalid;
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                "Timed out waiting for Redis pool connection"));
        }

        if (m_state == State::EnqueueFailed) {
            m_state = State::Invalid;
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Failed to enqueue Redis pool waiter"));
        }

        if (m_state == State::Error && m_error.has_value()) {
            auto error = std::move(*m_error);
            m_error.reset();
            m_state = State::Invalid;
            return std::unexpected(std::move(error));
        }

        m_state = State::Invalid;
        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Invalid Redis pool acquire state"));
    }

    void PoolAcquireAwaitable::markTimeout()
    {
        if (m_state == State::Creating && m_connect_awaitable.has_value()) {
            m_connect_awaitable->markTimeout();
            return;
        }
        if (m_state == State::Waiting) {
            if (m_waiter != nullptr) {
                if (!detail::try_complete_waiter(m_waiter->state,
                                                 detail::PoolWaiterState::TimedOut)) {
                    return;
                }
            }
            if (m_wait_counted && m_pool != nullptr) {
                decrementCounter(m_pool->m_waiting_requests);
                m_wait_counted = false;
            }
        }
        m_state = State::TimedOut;
    }

    // ======================== RedisConnectionPool 实现 ========================

#ifdef GALAY_SSL_FEATURE_ENABLED
    RedissPoolInitializeAwaitable::RedissPoolInitializeAwaitable(RedissConnectionPool& pool)
        : m_flow(std::make_unique<Flow>(pool))
        , m_inner(galay::kernel::AwaitableBuilder<Result, 4, Flow>(&m_controller, *m_flow)
                      .local<&Flow::run>()
                      .build())
    {
    }

    RedissPoolInitializeAwaitable::Flow::Flow(RedissConnectionPool& pool)
        : m_pool(&pool)
    {
    }

    void RedissPoolInitializeAwaitable::Flow::run(galay::kernel::SequenceOps<Result, 4>& ops)
    {
        auto& pool = *m_pool;
        ops.complete(pool.initializeSync());
    }

    RedissPoolAcquireAwaitable::RedissPoolAcquireAwaitable(RedissConnectionPool& pool)
        : m_pool(&pool)
        , m_start_time(std::chrono::steady_clock::now())
    {
    }

    RedissPoolAcquireAwaitable::SuspendAction
    RedissPoolAcquireAwaitable::prepareSuspend(galay::kernel::Waker waiter_waker)
    {
        m_start_time = std::chrono::steady_clock::now();
        if (m_pool == nullptr) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "TLS connection pool is missing");
            return SuspendAction::Error;
        }
        if (!m_pool->m_is_initialized) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "TLS connection pool not initialized");
            return SuspendAction::Error;
        }
        if (m_pool->m_is_shutting_down) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "TLS connection pool is shutting down");
            return SuspendAction::Error;
        }

        m_connection = m_pool->tryAcquireAvailable();
        if (m_connection) {
            incrementCounter(m_pool->m_total_acquired);
            incrementCounter(m_pool->m_active_connections);
            m_state = State::Ready;
        }

        if (m_state == State::Ready) {
        } else if ((m_connection = m_pool->createConnectionSlot())) {
            m_state = State::Creating;
        } else {
            m_waiter = std::make_shared<detail::RedissPoolWaiter>(std::move(waiter_waker));
            if (m_waiter == nullptr) {
                m_state = State::EnqueueFailed;
                return SuspendAction::Error;
            }
            if (!m_pool->enqueueWaiter(m_waiter)) {
                m_state = State::EnqueueFailed;
                m_waiter.reset();
                return SuspendAction::Error;
            }
            incrementCounter(m_pool->m_waiting_requests);
            m_wait_counted = true;
            m_state = State::Waiting;
            if (m_pool->m_idle_connections.load(std::memory_order_acquire) > 0) {
                const bool woke_waiter = m_pool->wakeOneWaiterFromAvailable();
                if (!woke_waiter && m_pool->m_idle_connections.load(std::memory_order_acquire) > 0) {
                    REDIS_LOG_DEBUG("[client]", "TLS Redis pool waiter queued while idle wake raced");
                }
            }
            return SuspendAction::Wait;
        }

        if (m_state == State::Ready) {
            m_pool->recordAcquireStats(m_start_time);
            return SuspendAction::Ready;
        }
        if (m_state != State::Creating || !m_connection) {
            m_state = State::Error;
            setAcquireError(m_error, RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR, "Invalid TLS connection acquire state");
            return SuspendAction::Error;
        }

        RedisConnectOptions options;
        options.username = m_pool->m_config.username;
        options.password = m_pool->m_config.password;
        options.db_index = m_pool->m_config.db_index;
        const bool connect_stored = emplaceOptional(
            m_connect_awaitable,
            m_connection->get()->connect(m_pool->m_config.host, m_pool->m_config.port, std::move(options)));
        if (!connect_stored) {
            m_state = State::Error;
            setAcquireError(m_error,
                            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                            "Failed to store TLS Redis connect awaitable");
            return SuspendAction::Error;
        }
        return SuspendAction::Connect;
    }

    RedissPoolAcquireAwaitable::Result RedissPoolAcquireAwaitable::await_resume()
    {
        if (m_state == State::Ready) {
            m_state = State::Invalid;
            return std::move(m_connection);
        }

        if (m_state == State::Creating) {
            if (!m_connect_awaitable.has_value() || !m_connection) {
                m_state = State::Invalid;
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                    "Missing TLS connect awaitable"));
            }

            auto connect_result = m_connect_awaitable->await_resume();
            m_connect_awaitable.reset();
            if (!connect_result) {
                if (m_pool != nullptr) {
                    m_pool->destroyConnectionSlot(m_connection);
                }
                m_state = State::Invalid;
                m_connection.reset();
                return std::unexpected(connect_result.error());
            }

            m_connection->setHealthy(true);
            m_connection->updateLastUsed();
            if (m_pool != nullptr) {
                incrementCounter(m_pool->m_total_acquired);
                incrementCounter(m_pool->m_active_connections);
                m_pool->recordAcquireStats(m_start_time);
            }
            m_state = State::Invalid;
            return std::move(m_connection);
        }

        if (m_state == State::Waiting) {
            if (m_wait_counted && m_pool != nullptr) {
                decrementCounter(m_pool->m_waiting_requests);
                m_wait_counted = false;
            }

            std::shared_ptr<PooledRedissConnection> connection;
            if (m_pool != nullptr && m_waiter != nullptr) {
                const auto waiter_state = m_waiter->state.load(std::memory_order_acquire);
                if (waiter_state == detail::PoolWaiterState::Completed) {
                    connection = std::move(m_waiter->connection);
                } else if (waiter_state == detail::PoolWaiterState::TimedOut) {
                    m_waiter.reset();
                    m_state = State::Invalid;
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                        "Timed out waiting for TLS Redis pool connection"));
                }
            }
            m_waiter.reset();
            m_state = State::Invalid;
            if (connection) {
                if (m_pool != nullptr) {
                    incrementCounter(m_pool->m_total_acquired);
                    incrementCounter(m_pool->m_active_connections);
                    m_pool->recordAcquireStats(m_start_time);
                }
                m_connection = std::move(connection);
                return std::move(m_connection);
            }
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Failed to acquire TLS connection after wakeup"));
        }

        if (m_state == State::TimedOut) {
            m_state = State::Invalid;
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                "Timed out waiting for TLS Redis pool connection"));
        }

        if (m_state == State::EnqueueFailed) {
            m_state = State::Invalid;
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Failed to enqueue TLS Redis pool waiter"));
        }

        if (m_state == State::Error && m_error.has_value()) {
            auto error = std::move(*m_error);
            m_error.reset();
            m_state = State::Invalid;
            return std::unexpected(std::move(error));
        }

        m_state = State::Invalid;
        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
            "Invalid TLS Redis pool acquire state"));
    }

    void RedissPoolAcquireAwaitable::markTimeout()
    {
        if (m_state == State::Creating && m_connect_awaitable.has_value()) {
            m_connect_awaitable->markTimeout();
            return;
        }
        if (m_state == State::Waiting) {
            if (m_waiter != nullptr) {
                if (!detail::try_complete_waiter(m_waiter->state,
                                                 detail::PoolWaiterState::TimedOut)) {
                    return;
                }
            }
            if (m_wait_counted && m_pool != nullptr) {
                decrementCounter(m_pool->m_waiting_requests);
                m_wait_counted = false;
            }
        }
        m_state = State::TimedOut;
    }
#endif

#endif // GALAY_REDIS_DETAILS_POOL_AWAITABLE_INL
