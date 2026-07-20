/**
 * @file awaitable.h
 * @brief etcd 异步客户端 Awaitable 类型定义
 * @author galay-etcd
 * @version 1.0.0
 */

#ifndef GALAY_ETCD_ASYNC_DETAILS_AWAITABLE_H
#define GALAY_ETCD_ASYNC_DETAILS_AWAITABLE_H

#include "../async/client.h"

namespace galay::etcd::details
{

using ConnectIoAwaitable =
    decltype(std::declval<galay::async::TcpSocket&>().connect(
        std::declval<const galay::kernel::Host&>()));
using CloseIoAwaitable = decltype(std::declval<galay::async::TcpSocket&>().close());
using HttpSerializedRequestAwaitable =
    decltype(std::declval<galay::http::HttpSession&>().sendSerializedRequest(
        std::declval<std::string>()));

/**
 * @brief IO Awaitable 基类模板
 * @tparam AwaitableType 底层 IO Awaitable 类型
 */
template <typename AwaitableType>
class IoAwaitableBase
{
protected:
    explicit IoAwaitableBase(AsyncEtcdClient& client)
        : m_client(&client)
    {
    }

    void startIo(AwaitableType&& awaitable)
    {
        m_awaitable.emplace(std::move(awaitable));
    }

    bool awaitReady() const noexcept
    {
        return !m_awaitable.has_value();
    }

    template <typename Promise>
    bool awaitSuspend(std::coroutine_handle<Promise> handle)
    {
        return m_awaitable->await_suspend(handle);
    }

    std::optional<AwaitableType>& awaitable()
    {
        return m_awaitable;
    }

    const std::optional<AwaitableType>& awaitable() const
    {
        return m_awaitable;
    }

    AsyncEtcdClient* m_client = nullptr;

private:
    std::optional<AwaitableType> m_awaitable;
};

/**
 * @brief 连接操作 Awaitable
 * @note 操作通过绑定客户端的 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdBoolResult 返回。
 */
class ConnectAwaitable
{
public:
    using Result = EtcdBoolResult;

    explicit ConnectAwaitable(AsyncEtcdClient& client);

    ConnectAwaitable(const ConnectAwaitable&) = delete;
    ConnectAwaitable& operator=(const ConnectAwaitable&) = delete;
    ConnectAwaitable(ConnectAwaitable&&) noexcept = default;
    ConnectAwaitable& operator=(ConnectAwaitable&&) noexcept = default;

    bool await_ready() noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner->await_suspend(handle);
    }
    Result await_resume();

private:
    enum class Phase {
        Connect,
        Done
    };

    struct SharedState {
        explicit SharedState(AsyncEtcdClient& client);

        galay::kernel::Host host;
        std::optional<Result> result;
        AsyncEtcdClient* client = nullptr;
        Phase phase = Phase::Done;
    };

    struct Machine {
        using result_type = Result;
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::Write;

        explicit Machine(std::shared_ptr<SharedState> state);

        galay::kernel::MachineAction<result_type> advance();
        void onConnect(std::expected<void, galay::kernel::IOError> result);
        void onRead(std::expected<size_t, galay::kernel::IOError>);
        void onWrite(std::expected<size_t, galay::kernel::IOError>);

    private:
        std::shared_ptr<SharedState> m_state;
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;

    std::shared_ptr<SharedState> m_state;
    std::unique_ptr<InnerAwaitable> m_inner;
};

/**
 * @brief 关闭连接 Awaitable
 * @note 操作通过绑定客户端的 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdBoolResult 返回。
 */
class CloseAwaitable : private IoAwaitableBase<CloseIoAwaitable>
{
public:
    explicit CloseAwaitable(AsyncEtcdClient& client);

    CloseAwaitable(const CloseAwaitable&) = delete;
    CloseAwaitable& operator=(const CloseAwaitable&) = delete;
    CloseAwaitable(CloseAwaitable&&) noexcept = default;
    CloseAwaitable& operator=(CloseAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdBoolResult await_resume();
};

/**
 * @brief JSON POST 请求 Awaitable
 * @note 操作通过绑定客户端的 IOScheduler 挂起推进，不阻塞调用线程；错误通过 expected 返回。
 */
class PostJsonAwaitable
{
public:
    PostJsonAwaitable(AsyncEtcdClient& client,
                      std::string api_path,
                      std::string body,
                      std::optional<std::chrono::milliseconds> force_timeout);

    PostJsonAwaitable(const PostJsonAwaitable&) = delete;
    PostJsonAwaitable& operator=(const PostJsonAwaitable&) = delete;
    PostJsonAwaitable(PostJsonAwaitable&&) noexcept = default;
    PostJsonAwaitable& operator=(PostJsonAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_ctx->awaitable.await_suspend(handle);
    }
    std::expected<std::string, EtcdError> await_resume();

private:
    struct Context
    {
        Context(AsyncEtcdClient& client,
                std::string api_path,
                std::string body);

        HttpSerializedRequestAwaitable awaitable;
        AsyncEtcdClient* owner = nullptr;
    };

    std::optional<Context> m_ctx;
};

/**
 * @brief JSON 操作 Awaitable 公共基类
 */
class JsonOpAwaitableBase
{
protected:
    explicit JsonOpAwaitableBase(AsyncEtcdClient& client);

    void startPost(std::string api_path,
                   std::string body,
                   std::optional<std::chrono::milliseconds> force_timeout = std::nullopt);
    bool awaitReady() const noexcept;
    template <typename Promise>
    bool awaitSuspend(std::coroutine_handle<Promise> handle)
    {
        return m_post_awaitable->await_suspend(handle);
    }
    std::expected<std::string, EtcdError> resumePost();

    std::optional<PostJsonAwaitable> m_post_awaitable;
    AsyncEtcdClient* m_client = nullptr;
};

/**
 * @brief Put 操作 Awaitable
 * @note 通过 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdBoolResult 返回。
 */
class PutAwaitable : private JsonOpAwaitableBase
{
public:
    PutAwaitable(AsyncEtcdClient& client,
                 std::string key,
                 std::string value,
                 std::optional<int64_t> lease_id);

    PutAwaitable(const PutAwaitable&) = delete;
    PutAwaitable& operator=(const PutAwaitable&) = delete;
    PutAwaitable(PutAwaitable&&) noexcept = default;
    PutAwaitable& operator=(PutAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdBoolResult await_resume();
};

/**
 * @brief Get 操作 Awaitable
 * @note 通过 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdGetResult 返回。
 */
class GetAwaitable : private JsonOpAwaitableBase
{
public:
    GetAwaitable(AsyncEtcdClient& client,
                 std::string key,
                 bool prefix,
                 std::optional<int64_t> limit);

    GetAwaitable(const GetAwaitable&) = delete;
    GetAwaitable& operator=(const GetAwaitable&) = delete;
    GetAwaitable(GetAwaitable&&) noexcept = default;
    GetAwaitable& operator=(GetAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdGetResult await_resume();
};

/**
 * @brief Delete 操作 Awaitable
 * @note 通过 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdDeleteResult 返回。
 */
class DeleteAwaitable : private JsonOpAwaitableBase
{
public:
    DeleteAwaitable(AsyncEtcdClient& client,
                    std::string key,
                    bool prefix);

    DeleteAwaitable(const DeleteAwaitable&) = delete;
    DeleteAwaitable& operator=(const DeleteAwaitable&) = delete;
    DeleteAwaitable(DeleteAwaitable&&) noexcept = default;
    DeleteAwaitable& operator=(DeleteAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdDeleteResult await_resume();
};

/**
 * @brief GrantLease 操作 Awaitable
 * @note 通过 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdLeaseGrantResult 返回。
 */
class GrantLeaseAwaitable : private JsonOpAwaitableBase
{
public:
    GrantLeaseAwaitable(AsyncEtcdClient& client, int64_t ttl_seconds);

    GrantLeaseAwaitable(const GrantLeaseAwaitable&) = delete;
    GrantLeaseAwaitable& operator=(const GrantLeaseAwaitable&) = delete;
    GrantLeaseAwaitable(GrantLeaseAwaitable&&) noexcept = default;
    GrantLeaseAwaitable& operator=(GrantLeaseAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdLeaseGrantResult await_resume();
};

/**
 * @brief KeepAlive 操作 Awaitable
 * @note 通过 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdLeaseGrantResult 返回。
 */
class KeepAliveAwaitable : private JsonOpAwaitableBase
{
public:
    KeepAliveAwaitable(AsyncEtcdClient& client, int64_t lease_id);

    KeepAliveAwaitable(const KeepAliveAwaitable&) = delete;
    KeepAliveAwaitable& operator=(const KeepAliveAwaitable&) = delete;
    KeepAliveAwaitable(KeepAliveAwaitable&&) noexcept = default;
    KeepAliveAwaitable& operator=(KeepAliveAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdLeaseGrantResult await_resume();

private:
    int64_t m_lease_id = 0;
};

/**
 * @brief Pipeline 操作 Awaitable
 * @note 通过 IOScheduler 挂起推进，不阻塞调用线程；错误通过 EtcdPipelineResult 返回。
 */
class PipelineAwaitable : private JsonOpAwaitableBase
{
public:
    PipelineAwaitable(AsyncEtcdClient& client, std::span<const PipelineOp> operations);
    PipelineAwaitable(AsyncEtcdClient& client, std::vector<PipelineOp> operations);

    PipelineAwaitable(const PipelineAwaitable&) = delete;
    PipelineAwaitable& operator=(const PipelineAwaitable&) = delete;
    PipelineAwaitable(PipelineAwaitable&&) noexcept = default;
    PipelineAwaitable& operator=(PipelineAwaitable&&) noexcept = default;

    bool await_ready() const noexcept;
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return awaitSuspend(handle);
    }
    EtcdPipelineResult await_resume();

private:
    std::vector<PipelineOpType> m_operation_types;
};

} // namespace galay::etcd::details

#endif // GALAY_ETCD_ASYNC_DETAILS_AWAITABLE_H
