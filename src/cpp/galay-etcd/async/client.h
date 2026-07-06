/**
 * @file etcd_client.h
 * @brief etcd 异步客户端定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 基于 C++20 协程的异步 etcd 客户端实现，支持：
 *          - 连接管理(Connect/Close)
 *          - KV 操作(Put/Get/Delete)
 *          - 租约操作(GrantLease/KeepAlive)
 *          - Pipeline 事务
 *          - Watch 事件监听
 *          通过 Builder 模式构建客户端，所有操作返回 Awaitable 对象，
 *          可在 co_await 表达式中使用。
 */

#ifndef GALAY_ETCD_ASYNC_ETCD_CLIENT_H
#define GALAY_ETCD_ASYNC_ETCD_CLIENT_H

#include "../base/etcd_config.h"
#include "../base/etcd_error.h"
#include "../base/network_cfg.h"
#include "../base/etcd_types.h"
#include "../base/etcd_value.h"
#include "../cluster/etcd_cluster_client.h"

#include "../../galay-http/kernel/http_session.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-kernel/common/host.hpp"
#include "../../galay-kernel/core/io_scheduler.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <coroutine>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace galay::etcd
{

/**
 * @brief etcd 异步客户端
 * @details 基于 C++20 协程和 galay-kernel IO 调度器的异步 etcd 客户端，
 *          通过 HTTP 协议与 etcd v3 REST API 通信。
 *          所有操作均返回 Awaitable 对象，支持 co_await 调用。
 * @note 该类不可拷贝、不可移动，生命周期内绑定固定的 IO 调度器。
 */
class AsyncEtcdClient
{
public:
    using PipelineOpType = galay::etcd::PipelineOpType;         ///< Pipeline 操作类型
    using PipelineOp = galay::etcd::PipelineOp;                 ///< Pipeline 操作描述
    using PipelineItemResult = galay::etcd::PipelineItemResult; ///< Pipeline 操作结果
    using WatchTaskHandler = std::function<galay::kernel::Task<void>(EtcdWatchResponse)>;   ///< Watch 协程回调处理器
    using WatchFunctionHandler = std::function<void(EtcdWatchResponse)>;                    ///< Watch 普通函数回调处理器

private:
    using ConnectIoAwaitable =
        decltype(std::declval<galay::async::TcpSocket&>().connect(std::declval<const galay::kernel::Host&>()));
    using CloseIoAwaitable = decltype(std::declval<galay::async::TcpSocket&>().close());
    using HttpSerializedRequestAwaitable =
        decltype(std::declval<galay::http::HttpSession&>().sendSerializedRequest(
            std::declval<std::string>()));

    /**
     * @brief IO Awaitable 基类模板
     * @tparam AwaitableType 底层 IO Awaitable 类型
     * @details 为底层 IO 操作提供统一的协程接口封装
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

public:
    /**
     * @brief 连接操作 Awaitable
     * @details 封装与 etcd 服务端的 TCP 连接建立过程，
     *          返回布尔值表示连接是否成功。
     */
    class ConnectAwaitable
    {
    public:
        using Result = EtcdBoolResult; ///< 操作结果类型

        /**
         * @brief 构造连接 Awaitable
         * @param client 异步客户端引用
         */
        ConnectAwaitable(AsyncEtcdClient& client);

        ConnectAwaitable(const ConnectAwaitable&) = delete;
        ConnectAwaitable& operator=(const ConnectAwaitable&) = delete;
        ConnectAwaitable(ConnectAwaitable&&) noexcept = default;
        ConnectAwaitable& operator=(ConnectAwaitable&&) noexcept = default;

        bool await_ready() noexcept; ///< 检查连接是否已完成
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return m_inner->await_suspend(handle);
        }
        Result await_resume(); ///< 获取连接结果

    private:
        /**
         * @brief 连接状态机阶段
         */
        enum class Phase {
            Connect, ///< 正在连接
            Done     ///< 连接完成
        };

        /**
         * @brief 连接操作的共享状态
         */
        struct SharedState {
            explicit SharedState(AsyncEtcdClient& client);

            galay::kernel::Host host;                 ///< 目标主机
            std::optional<Result> result;             ///< 连接结果
            AsyncEtcdClient* client = nullptr;       ///< 客户端指针
            Phase phase = Phase::Done;                ///< 当前阶段
        };

        /**
         * @brief 连接状态机
         * @details 驱动 TCP 连接的建立过程，处理连接回调
         */
        struct Machine {
            using result_type = Result;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::Write;

            explicit Machine(std::shared_ptr<SharedState> state);

            galay::kernel::MachineAction<result_type> advance(); ///< 推进状态机
            void onConnect(std::expected<void, galay::kernel::IOError> result); ///< 连接回调
            void onRead(std::expected<size_t, galay::kernel::IOError>);   ///< 读取回调
            void onWrite(std::expected<size_t, galay::kernel::IOError>);  ///< 写入回调

        private:
            std::shared_ptr<SharedState> m_state;
        };

        using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;

        std::shared_ptr<SharedState> m_state;
        std::unique_ptr<InnerAwaitable> m_inner;
    };

    /**
     * @brief 关闭连接 Awaitable
     * @details 封装 TCP 连接关闭过程，返回布尔值表示是否成功关闭。
     */
    class CloseAwaitable : private IoAwaitableBase<CloseIoAwaitable>
    {
    public:
        /**
         * @brief 构造关闭 Awaitable
         * @param client 异步客户端引用
         */
        CloseAwaitable(AsyncEtcdClient& client);

        CloseAwaitable(const CloseAwaitable&) = delete;
        CloseAwaitable& operator=(const CloseAwaitable&) = delete;
        CloseAwaitable(CloseAwaitable&&) noexcept = default;
        CloseAwaitable& operator=(CloseAwaitable&&) noexcept = default;

        bool await_ready() const noexcept; ///< 检查关闭是否已完成
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return awaitSuspend(handle);
        }
        EtcdBoolResult await_resume(); ///< 获取关闭结果
    };

    /**
     * @brief JSON POST 请求 Awaitable
     * @details 封装向 etcd 服务端发送 JSON POST 请求并接收响应的过程。
     */
    class PostJsonAwaitable
    {
    public:
        /**
         * @brief 构造 JSON POST Awaitable
         * @param client 异步客户端引用
         * @param api_path API 路径
         * @param body JSON 请求体
         * @param force_timeout 可选的强制超时时间
         */
        PostJsonAwaitable(AsyncEtcdClient& client,
                          std::string api_path,
                          std::string body,
                          std::optional<std::chrono::milliseconds> force_timeout);

        PostJsonAwaitable(const PostJsonAwaitable&) = delete;
        PostJsonAwaitable& operator=(const PostJsonAwaitable&) = delete;
        PostJsonAwaitable(PostJsonAwaitable&&) noexcept = default;
        PostJsonAwaitable& operator=(PostJsonAwaitable&&) noexcept = default;

        bool await_ready() const noexcept; ///< 检查请求是否已完成
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return m_ctx->awaitable.await_suspend(handle);
        }
        std::expected<std::string, EtcdError> await_resume(); ///< 获取响应体或错误

    private:
        /**
         * @brief POST 请求上下文
         */
        struct Context
        {
            AsyncEtcdClient* owner = nullptr;            ///< 客户端指针
            HttpSerializedRequestAwaitable awaitable;    ///< HTTP 请求 Awaitable

            Context(AsyncEtcdClient& client,
                    std::string api_path,
                    std::string body);
        };
        std::optional<Context> m_ctx;
    };

private:
    /**
     * @brief JSON 操作 Awaitable 基类
     * @details 为所有基于 JSON POST 的 etcd 操作(Put/Get/Delete/Lease/Pipeline)
     *          提供统一的 POST 请求发起和结果解析框架。
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

        AsyncEtcdClient* m_client = nullptr;
        std::optional<PostJsonAwaitable> m_post_awaitable;
    };

public:
    /**
     * @brief Put 操作 Awaitable
     * @details 向 etcd 写入一个键值对，可选绑定租约 ID。
     */
    class PutAwaitable : private JsonOpAwaitableBase
    {
    public:
        /**
         * @brief 构造 Put Awaitable
         * @param client 异步客户端引用
         * @param key 键名
         * @param value 值
         * @param lease_id 可选的租约 ID
         */
        PutAwaitable(AsyncEtcdClient& client,
                     std::string key,
                     std::string value,
                     std::optional<int64_t> lease_id);

        PutAwaitable(const PutAwaitable&) = delete;
        PutAwaitable& operator=(const PutAwaitable&) = delete;
        PutAwaitable(PutAwaitable&&) noexcept = default;
        PutAwaitable& operator=(PutAwaitable&&) noexcept = default;

        bool await_ready() const noexcept; ///< 检查 Put 操作是否已完成
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return awaitSuspend(handle);
        }
        EtcdBoolResult await_resume(); ///< 获取 Put 操作结果
    };

    /**
     * @brief Get 操作 Awaitable
     * @details 从 etcd 读取键值对，支持精确匹配和前缀查询。
     */
    class GetAwaitable : private JsonOpAwaitableBase
    {
    public:
        /**
         * @brief 构造 Get Awaitable
         * @param client 异步客户端引用
         * @param key 键名
         * @param prefix 是否为前缀查询
         * @param limit 返回数量限制
         */
        GetAwaitable(AsyncEtcdClient& client,
                     std::string key,
                     bool prefix,
                     std::optional<int64_t> limit);

        GetAwaitable(const GetAwaitable&) = delete;
        GetAwaitable& operator=(const GetAwaitable&) = delete;
        GetAwaitable(GetAwaitable&&) noexcept = default;
        GetAwaitable& operator=(GetAwaitable&&) noexcept = default;

        bool await_ready() const noexcept; ///< 检查 Get 操作是否已完成
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return awaitSuspend(handle);
        }
        EtcdGetResult await_resume(); ///< 获取查询到的键值对列表
    };

    /**
     * @brief Delete 操作 Awaitable
     * @details 从 etcd 删除键值对，支持精确删除和前缀删除。
     */
    class DeleteAwaitable : private JsonOpAwaitableBase
    {
    public:
        /**
         * @brief 构造 Delete Awaitable
         * @param client 异步客户端引用
         * @param key 键名
         * @param prefix 是否为前缀删除
         */
        DeleteAwaitable(AsyncEtcdClient& client,
                        std::string key,
                        bool prefix);

        DeleteAwaitable(const DeleteAwaitable&) = delete;
        DeleteAwaitable& operator=(const DeleteAwaitable&) = delete;
        DeleteAwaitable(DeleteAwaitable&&) noexcept = default;
        DeleteAwaitable& operator=(DeleteAwaitable&&) noexcept = default;

        bool await_ready() const noexcept; ///< 检查 Delete 操作是否已完成
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return awaitSuspend(handle);
        }
        EtcdDeleteResult await_resume(); ///< 获取删除数量
    };

    /**
     * @brief GrantLease 操作 Awaitable
     * @details 向 etcd 申请一个指定 TTL 的租约。
     */
    class GrantLeaseAwaitable : private JsonOpAwaitableBase
    {
    public:
        /**
         * @brief 构造 GrantLease Awaitable
         * @param client 异步客户端引用
         * @param ttl_seconds 租约存活时间（秒）
         */
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
        EtcdLeaseGrantResult await_resume(); ///< 获取分配的租约 ID
    };

    /**
     * @brief KeepAlive 操作 Awaitable
     * @details 向 etcd 发送一次租约续期请求。
     */
    class KeepAliveAwaitable : private JsonOpAwaitableBase
    {
    public:
        /**
         * @brief 构造 KeepAlive Awaitable
         * @param client 异步客户端引用
         * @param lease_id 需要续期的租约 ID
         */
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
        EtcdLeaseGrantResult await_resume(); ///< 获取续期后的租约 ID

    private:
        int64_t m_lease_id = 0; ///< 待续期的租约 ID
    };

    /**
     * @brief Pipeline 事务 Awaitable
     * @details 将多个操作(Put/Get/Delete)打包为 etcd 事务(Txn)一次性提交。
     */
    class PipelineAwaitable : private JsonOpAwaitableBase
    {
    public:
        /**
         * @brief 构造 Pipeline Awaitable（span 版本）
         * @param client 异步客户端引用
         * @param operations Pipeline 操作列表
         */
        PipelineAwaitable(AsyncEtcdClient& client, std::span<const PipelineOp> operations);
        /**
         * @brief 构造 Pipeline Awaitable（vector 版本）
         * @param client 异步客户端引用
         * @param operations Pipeline 操作列表
         */
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
        EtcdPipelineResult await_resume(); ///< 获取 Pipeline 操作结果列表

    private:
        std::vector<PipelineOpType> m_operation_types; ///< 各操作对应的类型列表
    };

    /**
     * @brief 构造异步 etcd 客户端
     * @param scheduler IO 调度器指针
     * @param config 异步客户端配置
     */
    AsyncEtcdClient(galay::kernel::IOScheduler* scheduler,
                    EtcdConfig config = {});
    ~AsyncEtcdClient(); ///< 析构函数，关闭连接并停止 Watch

    AsyncEtcdClient(const AsyncEtcdClient&) = delete;
    AsyncEtcdClient& operator=(const AsyncEtcdClient&) = delete;
    AsyncEtcdClient(AsyncEtcdClient&&) = delete;
    AsyncEtcdClient& operator=(AsyncEtcdClient&&) = delete;

    /**
     * @brief 异步连接到 etcd 服务端
     * @return 连接操作 Awaitable，co_await 返回 EtcdBoolResult
     */
    ConnectAwaitable connect();

    /**
     * @brief 异步关闭与 etcd 服务端的连接
     * @return 关闭操作 Awaitable，co_await 返回 EtcdBoolResult
     */
    CloseAwaitable close();

    /**
     * @brief 异步写入键值对
     * @param key 键名
     * @param value 值
     * @param lease_id 可选的租约 ID，绑定后键在租约过期时自动删除
     * @return Put 操作 Awaitable，co_await 返回 EtcdBoolResult
     */
    PutAwaitable put(const std::string& key,
                     const std::string& value,
                     std::optional<int64_t> lease_id = std::nullopt);

    /**
     * @brief 异步读取键值对
     * @param key 键名
     * @param prefix 是否为前缀查询，为 true 时返回所有以 key 为前缀的键值对
     * @param limit 返回数量限制
     * @return Get 操作 Awaitable，co_await 返回 EtcdGetResult
     */
    GetAwaitable get(const std::string& key,
                     bool prefix = false,
                     std::optional<int64_t> limit = std::nullopt);

    /**
     * @brief 异步删除键值对
     * @param key 键名
     * @param prefix 是否为前缀删除
     * @return Delete 操作 Awaitable，co_await 返回 EtcdDeleteResult（删除数量）
     */
    DeleteAwaitable del(const std::string& key, bool prefix = false);

    /**
     * @brief 异步申请租约
     * @param ttl_seconds 租约存活时间（秒）
     * @return GrantLease 操作 Awaitable，co_await 返回 EtcdLeaseGrantResult（租约 ID）
     */
    GrantLeaseAwaitable grantLease(int64_t ttl_seconds);

    /**
     * @brief 异步发送一次租约续期请求
     * @param lease_id 需要续期的租约 ID
     * @return KeepAlive 操作 Awaitable，co_await 返回 EtcdLeaseGrantResult（租约 ID）
     */
    KeepAliveAwaitable keepAliveOnce(int64_t lease_id);

    /**
     * @brief 异步执行 Pipeline 事务（span 版本）
     * @param operations Pipeline 操作列表
     * @return Pipeline 操作 Awaitable，co_await 返回 EtcdPipelineResult
     */
    PipelineAwaitable pipeline(std::span<const PipelineOp> operations);

    /**
     * @brief 异步执行 Pipeline 事务（vector 版本）
     * @param operations Pipeline 操作列表
     * @return Pipeline 操作 Awaitable，co_await 返回 EtcdPipelineResult
     */
    PipelineAwaitable pipeline(std::vector<PipelineOp> operations);

    /**
     * @brief 获取统计快照
     * @details 当前异步 client 暂未在普通单端点路径中累计指标，
     *          返回值用于锁定公开 API 形状，后续生产 wrapper 会补齐计数。
     * @return 当前统计快照
     */
    [[nodiscard]] EtcdClientStats getStats() const
    {
        return m_stats;
    }

    /**
     * @brief 为单个 key 启动异步 watch，并把每个事件批次投递给 `Task<void>` 处理器。
     * @note handler 参数按值传递，避免协程 frame 持有悬空引用。
     * @note watch 由后台线程维持长连接；处理器本身会被调度到 client 绑定的 `IOScheduler`。
     */
    EtcdBoolResult watch(const std::string& key, WatchTaskHandler handler);

    /**
     * @brief 为单个 key 启动异步 watch，并在后台 watch 线程上直接调用普通函数处理器。
     * @note 如果需要在 `IOScheduler` 上继续协程化处理，请使用 `WatchTaskHandler` 重载。
     */
    EtcdBoolResult watch(const std::string& key, WatchFunctionHandler handler);

    /**
     * @brief 检查客户端是否已连接
     * @return 已连接返回 true，否则返回 false
     */
    [[nodiscard]] bool connected() const;

private:
    struct WatchWorkerState;

    void resetLastOperation();
    void setError(EtcdErrorType type, const std::string& message);
    void setError(EtcdError error);

    [[nodiscard]] EtcdBoolResult currentBoolResult() const;
    std::expected<std::string, EtcdError> resumePostOrCurrent(
        std::optional<PostJsonAwaitable>& post_awaitable);
    [[nodiscard]] std::string buildSerializedPostRequest(std::string_view api_path,
                                                         std::string_view body) const;

    PostJsonAwaitable postJsonInternal(const std::string& api_path,
                                       const std::string& body,
                                       std::optional<std::chrono::milliseconds> force_timeout = std::nullopt);
    EtcdBoolResult startWatchWorker(const std::string& key,
                                    std::function<void(EtcdWatchResponse)> dispatch);
    void stopWatchWorkers();
    void joinWatchWorkers();

private:
    galay::kernel::IOScheduler* m_scheduler;                              ///< IO 调度器指针
    EtcdConfig m_config;                                                   ///< 客户端配置
    EtcdNetworkConfig m_network_config;                                    ///< 网络配置
    std::string m_api_prefix;                                              ///< API 路径前缀
    std::string m_host_header;                                             ///< HTTP Host 头
    std::string m_serialized_request_prefix;                               ///< 序列化请求前缀缓存
    std::string m_serialized_request_headers;                              ///< 序列化请求头缓存
    std::optional<galay::kernel::Host> m_server_host;                      ///< 解析后的服务端主机地址
    std::string m_endpoint_error;                                          ///< 端点解析错误信息

    std::unique_ptr<galay::async::TcpSocket> m_socket;                     ///< TCP 套接字
    std::unique_ptr<galay::http::HttpSession> m_http_session;              ///< HTTP 会话

    EtcdError m_last_error;                                                ///< 最后一次错误
    EtcdClientStats m_stats{};                                             ///< 统计快照占位
    std::mutex m_watch_mutex;                                              ///< Watch 工作列表互斥锁
    std::vector<std::shared_ptr<WatchWorkerState>> m_watch_workers;        ///< Watch 工作线程状态列表
    galay::kernel::IPType m_ip_type = galay::kernel::IPType::IPV4;        ///< IP 协议类型
    bool m_endpoint_valid = false;                                         ///< 端点地址是否有效
    bool m_connected = false;                                              ///< 是否已连接
};

/**
 * @brief 异步 etcd 客户端构建器
 * @details 使用 Builder 模式逐步配置并构建 AsyncEtcdClient 实例。
 *          支持链式调用设置调度器、端点地址、API 前缀、超时时间等参数。
 *
 * @code
 * auto client = AsyncEtcdClientBuilder()
 *     .scheduler(&scheduler)
 *     .endpoint("http://127.0.0.1:2379")
 *     .requestTimeout(std::chrono::seconds(5))
 *     .build();
 * @endcode
 */
class AsyncEtcdClientBuilder
{
public:
    /**
     * @brief 设置 IO 调度器
     * @param scheduler IO 调度器指针
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler)
    {
        m_scheduler = scheduler;
        return *this;
    }

    /**
     * @brief 设置 etcd 服务端点地址
     * @param endpoint 端点地址，格式为 http(s)://host:port
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& endpoint(std::string endpoint)
    {
        m_config.endpoint = std::move(endpoint);
        return *this;
    }

    /**
     * @brief 设置 API 路径前缀
     * @param prefix 路径前缀，如 "/v3"
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& apiPrefix(std::string prefix)
    {
        m_config.api_prefix = std::move(prefix);
        return *this;
    }

    /**
     * @brief 设置生产模式配置
     * @param config 多端点、重试与健康检查配置
     * @return 构建器引用，支持链式调用
     * @note 当前仅保存配置并在 endpoints 非空时同步首个 endpoint，
     *       不改变现有单端点请求行为。
     */
    AsyncEtcdClientBuilder& productionConfig(EtcdProductionConfig config)
    {
        if (!config.endpoints.empty()) {
            m_config.endpoint = config.endpoints.front();
        }
        m_config.production = std::move(config);
        return *this;
    }

    /**
     * @brief 设置请求超时时间
     * @param timeout 超时时间
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& requestTimeout(std::chrono::milliseconds timeout)
    {
        m_config.request_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置网络缓冲区大小
     * @param size 缓冲区大小（字节）
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    /**
     * @brief 设置是否启用 TCP keepalive
     * @param enabled 是否启用
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& keepAlive(bool enabled)
    {
        m_config.keepalive = enabled;
        return *this;
    }

    /**
     * @brief 设置连接 socket 是否启用 TCP_NODELAY
     * @param enabled true 表示启用 TCP_NODELAY；false 表示保留系统默认
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& tcpNoDelay(bool enabled)
    {
        m_config.tcp_no_delay = enabled;
        return *this;
    }

    /**
     * @brief 直接设置完整的客户端配置
     * @param config 客户端配置
     * @return 构建器引用，支持链式调用
     */
    AsyncEtcdClientBuilder& config(EtcdConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    /**
     * @brief 构建异步 etcd 客户端
     * @return 配置好的 AsyncEtcdClient 实例
     */
    AsyncEtcdClient build() const;

    /**
     * @brief 仅构建配置对象而不创建客户端
     * @return 配置好的 EtcdConfig 实例
     */
    EtcdConfig& buildConfig()
    {
        return m_config;
    }

    /**
     * @brief 仅查看构建配置对象而不创建客户端
     * @return 当前配置引用
     */
    const EtcdConfig& buildConfig() const
    {
        return m_config;
    }

private:
    galay::kernel::IOScheduler* m_scheduler = nullptr; ///< IO 调度器指针
    EtcdConfig m_config{};                             ///< 客户端配置
};

/**
 * @brief async cluster wrapper 单次尝试快照
 * @details 仅承载离线 policy loop 需要的信息，不触发真实网络 I/O。
 *          `config` 是调用侧随后真正发起 I/O 时应使用的 endpoint 绑定配置副本。
 */
struct AsyncEtcdClusterAttempt
{
    size_t endpoint_index = 0;                                           ///< 本次选择的 endpoint 下标
    size_t attempt = 0;                                                  ///< 当前请求内的第几次尝试，从 0 开始
    EtcdConfig config{};                                                 ///< 已绑定 endpoint 的配置副本
    std::chrono::milliseconds backoff = std::chrono::milliseconds::zero(); ///< 当前尝试前应遵守的退避时间
};

/**
 * @brief async etcd cluster client 的最小离线 wrapper
 * @details 当前仅暴露 coroutine-friendly 的端点选择与重试控制面。
 *          不做真实网络请求，不提供 async cluster `connect/put/get/del` API，
 *          也不引入后台 health probe 线程。
 */
class AsyncEtcdClusterClient
{
public:
    using Attempt = AsyncEtcdClusterAttempt;
    using AttemptResult = std::expected<Attempt, EtcdError>;
    using AttemptAwaitable = galay::kernel::ReadyAwaitable<AttemptResult>;

    explicit AsyncEtcdClusterClient(galay::kernel::IOScheduler* scheduler = nullptr,
                                    EtcdConfig config = {});

    /**
     * @brief 开始一次离线 cluster 尝试规划
     * @return 立即就绪的 AttemptAwaitable，成功时返回首个应尝试的 endpoint 配置
     */
    [[nodiscard]] AttemptAwaitable beginAttempt();

    /**
     * @brief 根据上一次错误规划下一次离线重试
     * @param previous 上一次尝试快照
     * @param error 上一次尝试对应的错误分类
     * @return 立即就绪的 AttemptAwaitable；失败时直接返回 fail-fast 错误
     * @note 调用侧负责使用返回的 `Attempt.config` 执行真实 I/O。
     */
    [[nodiscard]] AttemptAwaitable nextAttempt(const Attempt& previous, EtcdError error);

    /**
     * @brief 记录一次由调用侧完成的成功尝试
     * @param attempt 成功的尝试快照
     * @param when 成功时间
     * @note 该方法只更新 health snapshot / stats，不执行任何网络操作。
     */
    void markSuccess(
        const Attempt& attempt,
        std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

    [[nodiscard]] const std::vector<EtcdEndpointHealthSnapshot>& getEndpointSnapshots() const;
    [[nodiscard]] EtcdClientStats getStats() const;
    [[nodiscard]] galay::kernel::IOScheduler* scheduler() const;

private:
    [[nodiscard]] EtcdConfig configForEndpoint(size_t index) const;
    [[nodiscard]] AttemptResult makeAttempt(
        size_t attempt,
        std::chrono::milliseconds backoff);

private:
    galay::kernel::IOScheduler* m_scheduler = nullptr;
    EtcdConfig m_config{};
    EtcdClusterState m_state;
};

/**
 * @brief async cluster wrapper 构建器
 * @details 保持与 `AsyncEtcdClientBuilder` 接近的配置 surface，
 *          但当前 build 出来的对象只负责离线 policy loop，
 *          不代表已经具备真实 async cluster KV I/O 能力。
 */
class AsyncEtcdClusterClientBuilder
{
public:
    AsyncEtcdClusterClientBuilder()
    {
        m_config.endpoint.clear();
    }

    AsyncEtcdClusterClientBuilder& scheduler(galay::kernel::IOScheduler* scheduler)
    {
        m_scheduler = scheduler;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& endpoint(std::string endpoint)
    {
        m_config.endpoint = std::move(endpoint);
        return *this;
    }

    AsyncEtcdClusterClientBuilder& apiPrefix(std::string prefix)
    {
        m_config.api_prefix = std::move(prefix);
        return *this;
    }

    AsyncEtcdClusterClientBuilder& productionConfig(EtcdProductionConfig config)
    {
        if (!config.endpoints.empty()) {
            m_config.endpoint = config.endpoints.front();
        }
        m_config.production = std::move(config);
        return *this;
    }

    AsyncEtcdClusterClientBuilder& requestTimeout(std::chrono::milliseconds timeout)
    {
        m_config.request_timeout = timeout;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& keepAlive(bool enabled)
    {
        m_config.keepalive = enabled;
        return *this;
    }

    AsyncEtcdClusterClientBuilder& config(EtcdConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    AsyncEtcdClusterClient build() const;

    EtcdConfig& buildConfig()
    {
        return m_config;
    }

    const EtcdConfig& buildConfig() const
    {
        return m_config;
    }

private:
    galay::kernel::IOScheduler* m_scheduler = nullptr;
    EtcdConfig m_config{};
};

} // namespace galay::etcd

inline galay::etcd::AsyncEtcdClient galay::etcd::AsyncEtcdClientBuilder::build() const
{
    return AsyncEtcdClient(m_scheduler, m_config);
}

inline galay::etcd::AsyncEtcdClusterClient galay::etcd::AsyncEtcdClusterClientBuilder::build() const
{
    return AsyncEtcdClusterClient(m_scheduler, m_config);
}

#endif // GALAY_ETCD_ASYNC_ETCD_CLIENT_H
