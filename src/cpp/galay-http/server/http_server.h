/**
 * @file http_server.h
 * @brief HTTP/HTTPS 服务器，支持自定义连接处理器和路由模式
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HttpServerImpl 模板类，支持两种运行模式：
 *          1. 自定义连接处理器模式：用户完全控制连接处理逻辑
 *          2. 路由模式：框架驱动请求读取、Keep-Alive 循环与路由分发
 *          内部使用 Runtime 管理多线程 IO 调度，支持 SO_REUSEPORT 多线程 accept。
 */

#ifndef GALAY_HTTP_SERVER_H
#define GALAY_HTTP_SERVER_H

#include "../kernel/http_conn.h"
#include "http_router.h"
#include "http_policy.h"
#include "../common/http_log.h"
#include "../builder/http_builder.h"
#include "../utils/http_helper.h"
#include "../plugin/common/defn.h"
#include "../../galay-kernel/async/async_tcp.h"
#include "../../galay-kernel/core/runtime.h"
#include <memory>
#include <atomic>
#include <expected>
#include <functional>
#include <cstdint>
#include <optional>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_socket.h"
#include "../../galay-ssl/ssl/ssl_context.h"
#endif

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class HttpServerImpl;

/**
 * @brief HTTP连接处理器类型
 */
template<typename SocketType>
using HttpConnHandlerImpl = std::function<Task<void>(HttpConnImpl<SocketType>)>;

/**
 * @brief HTTP服务器配置
 * @details
 * - `host` / `port` / `backlog` 控制监听 socket
 * - `tcp_no_delay` 控制 accept 后的连接 socket 是否启用 TCP_NODELAY
 * - `io_scheduler_count` 与 `parallel_scheduler_count` 交由 `RuntimeBuilder` 创建调度器
 * - `affinity` 只描述调度器绑核策略，不会改变业务 handler 的语义
 * - `policy` 保存 route-mode 后续生产级加固所需的默认策略
 */
struct HttpServerConfig
{
    std::string host = "0.0.0.0";              ///< 监听地址
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO; ///< IO 调度器数量
    size_t parallel_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO; ///< 计算调度器数量
    RuntimeAffinityConfig affinity;             ///< 调度器绑核策略
    HttpServerPolicy policy;                     ///< HTTP/1.1 route-mode 生产策略
    int backlog = 128;                          ///< listen backlog 队列长度
    uint16_t port = 8080;                       ///< 监听端口
    bool tcp_no_delay = true;                   ///< 是否为已接受连接启用 TCP_NODELAY
};

/**
 * @brief HTTP 服务器 builder
 * @details builder 不持有线程或监听 socket；真正的 runtime 和监听资源在 `build()` 后的服务器实例中创建。
 */
class HttpServerBuilder {
public:
    HttpServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; } ///< 设置监听地址
    HttpServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; } ///< 设置监听端口
    HttpServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; } ///< 设置 listen backlog
    HttpServerBuilder& tcp_no_delay(bool v)               { m_config.tcp_no_delay = v; return *this; } ///< 设置已接受连接是否启用 TCP_NODELAY
    HttpServerBuilder& io_scheduler_count(size_t v)       { m_config.io_scheduler_count = v; return *this; } ///< 设置 IO 调度器数量
    HttpServerBuilder& parallel_scheduler_count(size_t v)  { m_config.parallel_scheduler_count = v; return *this; } ///< 设置计算调度器数量
    HttpServerBuilder& policy(HttpServerPolicy v)        { m_config.policy = std::move(v); return *this; } ///< 设置 route-mode 生产策略
    /**
     * @brief 设置顺序 CPU 亲和性
     * @param io_count IO 调度器绑定的 CPU 核心数
     * @param parallel_count 计算调度器绑定的 CPU 核心数
     * @return Builder 引用
     */
    HttpServerBuilder& sequential_affinity(size_t io_count, size_t parallel_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_parallel_count = parallel_count;
        return *this;
    }
    /**
     * @brief 设置自定义 CPU 亲和性
     * @param io_cpus IO 调度器绑定的 CPU 核心列表
     * @param parallel_cpus 计算调度器绑定的 CPU 核心列表
     * @return 成功返回 true，CPU 核心数与调度器数不匹配返回 false
     */
    bool custom_affinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> parallel_cpus) {
        if (io_cpus.size() != m_config.io_scheduler_count ||
            parallel_cpus.size() != m_config.parallel_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(io_cpus);
        m_config.affinity.custom_parallel_cpus = std::move(parallel_cpus);
        return true;
    }
    HttpServerImpl<AsyncTcpSocket> build() const; ///< 构建 HTTP 服务器实例
    HttpServerConfig build_config() const                { return m_config; } ///< 导出配置
private:
    HttpServerConfig m_config;
};

/**
 * @brief HTTP服务器模板类
 * @details
 * 典型调用方式有两种：
 * - `start(ConnHandler)`：调用方完全接管单连接处理逻辑
 * - `start(HttpRouter&&)`：由框架驱动请求读取、Keep-Alive 循环与路由分发
 *
 * 生命周期与线程说明：
 * - 服务器独占持有内部 `Runtime`
 * - `start()` 成功后会启动 runtime，并在每个 IO 调度器上创建监听/accept 循环
 * - `stop()` 可重复调用；第一次调用会关闭 listener 并停止 runtime
 *
 * 处理器约束：
 * - 传入的 `ConnHandler` / 路由 handler 必须在协程结束前完成连接相关资源的合法使用
 * - 对 `start(HttpRouter&&)` 路径，框架会在循环结束后统一关闭 `HttpConn`
 */
template<typename SocketType>
class HttpServerImpl
{
public:

    using HttpConnHandler = HttpConnHandlerImpl<SocketType>;

    explicit HttpServerImpl(const HttpServerConfig& config = HttpServerConfig())
        : m_runtime(RuntimeBuilder().io_scheduler_count(config.io_scheduler_count)
                                   .parallel_scheduler_count(config.parallel_scheduler_count)
                                   .apply_affinity(config.affinity)
                                   .build())
        , m_config(config)
        , m_handler(nullptr)
        , m_running(false)
    {
    }

    virtual ~HttpServerImpl() {
        stop();
    }

    HttpServerImpl(const HttpServerImpl&) = delete;
    HttpServerImpl& operator=(const HttpServerImpl&) = delete;

    /**
     * @brief 以自定义连接处理器启动服务器
     * @param handler 每个新连接都会被包装成 `Task<void>` 并交给该处理器
     * @note handler 必须可安全复制或移动到服务器内部，且不应捕获悬空引用
     */
    void start(HttpConnHandler handler) {
        m_handler = handler;
        start_internal();
    }

    /**
     * @brief 以路由模式启动服务器
     * @param router 将被移动到服务器内部保存的路由表
     * @details 框架会负责：
     * - 持续读取 HTTP 请求
     * - 处理 Keep-Alive / Connection: close
     * - 进行路由匹配和缺省 404 响应
     * - 在循环结束后关闭连接
     *
     * 该模式当前仅支持明文 `AsyncTcpSocket` 路由处理；HTTPS 仍应通过显式 handler 控制读写流程。
     */
    void start(HttpRouter&& router) {
        m_router = std::move(router);

        m_handler = [this](HttpConnImpl<SocketType> conn) -> Task<void> {
            bool keep_alive = true;
            size_t handled_requests = 0;
            HttpReaderSetting reader_setting;
            reader_setting.set_max_header_size(m_config.policy.request_limits.max_header_size);
            reader_setting.set_max_header_count(m_config.policy.request_limits.max_header_count);
            reader_setting.set_max_header_line_size(m_config.policy.request_limits.max_header_line_size);
            reader_setting.set_max_uri_size(m_config.policy.request_limits.max_uri_size);
            reader_setting.set_max_body_size(m_config.policy.request_limits.max_body_size);
            HttpWriterSetting writer_setting;
            writer_setting.set_send_timeout(
                static_cast<int>(m_config.policy.timeouts.response_write_timeout.count()));
            conn.set_default_writer_setting(writer_setting);

            while (keep_alive) {
                const bool waiting_for_initial_request = handled_requests == 0;
                const auto read_timeout = waiting_for_initial_request
                    ? m_config.policy.timeouts.request_header_timeout
                    : m_config.policy.keep_alive.keep_alive_idle_timeout;
                auto reader = conn.get_reader(reader_setting);
                HttpRequest request;
                auto read_result = co_await reader.get_request(request)
                    .timeout(read_timeout)
                    .body_timeout(m_config.policy.timeouts.request_body_timeout);

                if (!read_result) {
                    const auto& error = read_result.error();
                    bool is_disconnect_like = error.code() == kConnectionClose;
                    const bool is_timeout_like =
                        error.code() == kRecvTimeOut ||
                        error.code() == kSendTimeOut ||
                        error.code() == kRequestTimeOut;

                    if (is_disconnect_like) {
                        HTTP_LOG_DEBUG("[recv] [disconnect]", "code={}", static_cast<int>(error.code()));
                    } else if (is_timeout_like) {
                        HTTP_LOG_WARN("[recv] [timeout]", "code={} msg={}", static_cast<int>(error.code()), error.message());
                    } else {
                        HTTP_LOG_ERROR("[recv] [fail]", "code={} msg={}", static_cast<int>(error.code()), error.message());
                    }

                    const bool should_send_timeout_response =
                        is_timeout_like && waiting_for_initial_request;

                    if (!is_disconnect_like && !is_timeout_like) {
                        auto response = HttpHelper::default_http_response(error.to_http_status_code());
                        response.header().header_pairs().add_header_pair("Connection", "close");
                        auto writer = conn.get_writer();
                        auto write_result = co_await writer.send_response(response);
                        if (!write_result) {
                            HTTP_LOG_ERROR("[send] [fail]", "code={} msg={}",
                                          static_cast<int>(write_result.error().code()),
                                          write_result.error().message());
                        }
                    } else if (should_send_timeout_response) {
                        auto response = HttpHelper::default_http_response(error.to_http_status_code());
                        response.header().header_pairs().add_header_pair("Connection", "close");
                        auto writer = conn.get_writer();
                        auto write_result = co_await writer.send_response(response);
                        if (!write_result) {
                            HTTP_LOG_ERROR("[send] [fail]", "code={} msg={}",
                                          static_cast<int>(write_result.error().code()),
                                          write_result.error().message());
                        }
                    }
                    break;
                }

                keep_alive = request.header().is_keep_alive() && !request.header().is_connection_close();
                ++handled_requests;

                auto [handler, params] = m_router->find_handler(request.header().method(), request.header().uri());
                request.set_route_params(std::move(params));

                if (!handler && m_router->has_fallback_proxy()) {
                    handler = m_router->fallback_proxy_handler();
                }

                if (!handler) {

                    auto response = Http1_1ResponseBuilder()
                        .status(HttpStatusCode::NotFound_404)
                        .header("Content-Type", "text/plain")
                        .body("404 Not Found")
                        .build_move();

                    auto writer = conn.get_writer();
                    auto result = co_await writer.send_response(response);
                    if (!result) {
                        HTTP_LOG_ERROR("[send] [fail]", "code={} msg={}", static_cast<int>(result.error().code()), result.error().message());
                    }

                    if (!keep_alive) {
                        break;
                    }
                    continue;
                }

                if constexpr (std::is_same_v<SocketType, AsyncTcpSocket>) {
                    co_await (*handler)(conn, std::move(request));
                } else {
                    break;
                }

                if (!keep_alive) {
                    break;
                }
            }

            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_ERROR("[socket] [close-fail]",
                              "context=route-connection error={}",
                              close_result.error().message());
            }
            co_return;
        };

        start_internal();
    }

    /**
     * @brief 注册 accept 后、HTTP 连接包装前执行的插件
     * @param plugin 由 server 接管生命周期的插件实例
     * @return 注册成功返回 true；服务器已启动或 plugin 为空时返回 false
     * @details
     * - 必须在 `start(...)` 前调用，启动后不允许修改插件列表
     * - `start()` 在 runtime 启动后、accept loop 投递前按注册顺序调用
     * - `stop()` 在 runtime 停止前按注册反序调用
     * - `handle()` 返回 `false` 时停止后续插件，并跳过当前连接的业务处理
     */
    bool add_accept_plugin(std::unique_ptr<plugin::AcceptPlugin<SocketType>> plugin) {
        if (m_running.load() || !plugin) {
            return false;
        }

        m_accept_plugins.push_back(std::move(plugin));
        return true;
    }

    /**
     * @brief 停止服务器并关闭内部 runtime
     * @details 该函数幂等；当服务器未运行但已有插件启动时仍会尝试清理插件。
     */
    void stop() {
        if (!m_running.load()) {
            stop_started_plugins();
            m_listeners.clear();
            return;
        }

        m_running.store(false);

        stop_started_plugins();
        close_listeners();
        m_runtime.stop();
        m_listeners.clear();

    }

    /**
     * @brief 检查服务器是否正在运行
     * @return 运行中返回 true
     */
    bool is_running() const {
        return m_running.load();
    }

    /**
     * @brief 获取内部 Runtime 引用
     * @return Runtime 引用
     */
    Runtime& get_runtime() {
        return m_runtime;
    }

protected:
    /**
     * @brief 将 server 拥有的 root task 绑定到当前 Runtime 后提交到指定调度器。
     * @details `RuntimeHandle::current()` 依赖 TaskState 中的 runtime 指针；server
     *          自己使用裸 scheduler 投递时必须显式绑定，否则路由 handler 内无法
     *          安全使用 RuntimeHandle 派生 blocking task。
     */
    template <typename T>
    bool schedule_runtime_task(Scheduler* scheduler, Task<T> task) {
        if (scheduler == nullptr || !task.is_valid()) {
            return false;
        }

        TaskRef task_ref = galay::kernel::detail::TaskAccess::detach_task(std::move(task));
        galay::kernel::detail::set_task_runtime(task_ref, &m_runtime);
        galay::kernel::detail::set_task_scheduler(task_ref, scheduler);
        return scheduler->schedule(std::move(task_ref));
    }

    Task<void> close_listener(AsyncTcpSocket* listener) {
        if (listener == nullptr || listener->handle() == GHandle::invalid()) {
            co_return;
        }
        auto close_result = co_await listener->close();
        if (!close_result && close_result.error().code() != kClosed) {
            HTTP_LOG_WARN("[socket] [close-fail]",
                          "context=server-listener error={}",
                          close_result.error().message());
        }
        co_return;
    }

    void close_listeners() {
        std::vector<JoinHandle<void>> pending;
        pending.reserve(m_listeners.size());
        for (size_t i = 0; i < m_listeners.size(); ++i) {
            auto* scheduler = m_runtime.get_io_scheduler(i);
            auto task = close_listener(&m_listeners[i]);
            if (scheduler == nullptr || !task.is_valid()) {
                continue;
            }
            const TaskRef& task_ref = galay::kernel::detail::TaskAccess::task_ref(task);
            galay::kernel::detail::set_task_runtime(task_ref, &m_runtime);
            galay::kernel::detail::set_task_scheduler(task_ref, scheduler);
            if (scheduler->schedule(task_ref)) {
                pending.emplace_back(
                    galay::kernel::detail::TaskAccess::detach_task(std::move(task)));
            }
        }
        for (const auto& task : pending) {
            auto wait_result = task.wait();
            if (!wait_result) {
                HTTP_LOG_WARN("[runtime] [wait-fail]", "context=server-listener-close");
            }
        }
    }

    /**
     * @brief 内部启动实现
     * @return 成功返回 true
     * @details 初始化 runtime 并在每个 IO 调度器上启动 server_loop
     */
    virtual bool start_internal() {
        if (m_running.load()) {
            return false;
        }

        if (!m_handler) {
            return false;
        }


        auto runtime_start = m_runtime.start();
        if (!runtime_start.has_value()) {
            HTTP_LOG_ERROR("[runtime] [start-fail]",
                           "error={}",
                           runtime_start.error().message());
            return false;
        }

        if (!start_plugins()) {
            m_runtime.stop();
            return false;
        }

        size_t io_scheduler_count = m_runtime.get_io_scheduler_count();
        m_listeners.clear();
        m_listeners.reserve(io_scheduler_count);
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto listener = create_listener_socket();
            if (!listener) {
                HTTP_LOG_ERROR("[socket] [listen-fail]",
                               "error={}",
                               listener.error().message());
                stop_started_plugins();
                m_runtime.stop();
                m_listeners.clear();
                return false;
            }
            m_listeners.push_back(std::move(*listener));
        }

        m_running.store(true);

        // 在每个 IO 调度器上启动一个 server_loop，每个 server_loop 由 server 持有独立 listener。
        // stop() 会先清空 m_listeners，同步关闭监听 fd，再停止 runtime。
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.get_io_scheduler(i);
            if (scheduler) {
                if (!schedule_runtime_task(scheduler, server_loop(scheduler, &m_listeners[i]))) {
                    HTTP_LOG_ERROR("[runtime] [schedule-fail]",
                                   "context=server-loop index={}",
                                   i);
                    m_running.store(false);
                    stop_started_plugins();
                    m_runtime.stop();
                    m_listeners.clear();
                    return false;
                }
            }
        }

        return true;
    }

    /**
     * @brief 服务器 accept 循环
     * @param scheduler 当前 IO 调度器
     * @details 每个 IO 调度器上运行一个独立的 server_loop，
     *          创建独立的 listener socket，利用 SO_REUSEPORT 实现多线程 accept。
     */
    virtual Task<void> server_loop(IOScheduler* scheduler, AsyncTcpSocket* listener) {
        if (listener == nullptr) {
            co_return;
        }


        // 阶段 7：主 accept 循环，运行期间持续等待新连接
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener->accept(&client_host);

            // 阶段 8：处理 accept 失败，服务器仍运行时记录告警并继续循环
            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_WARN("[accept] [fail]", "error={}", accept_result.error().message());
                }
                continue;
            }


            // 阶段 9：根据 accept 得到的句柄构造协议层客户端 socket
            auto client_socket_opt = create_client_socket(*accept_result);
            if (!client_socket_opt) {
                continue;
            }


            // 阶段 10：配置客户端 socket 为非阻塞模式，并按配置选择 TCP_NODELAY
            SocketType client_socket = std::move(*client_socket_opt);
            auto nonblock_result = client_socket.option().handle_non_block();
            if (!nonblock_result) {
                continue;
            }
            if (m_config.tcp_no_delay) {
                auto nodelay_result = client_socket.option().handle_tcp_no_delay();
                if (!nodelay_result) {
                    HTTP_LOG_DEBUG("[socket] [nodelay]", "failed to set TCP_NODELAY");
                }
            }

            // 阶段 11：执行 accept plugin，允许用户在进入连接处理前拦截 socket
            auto continuing_result = co_await run_accept_plugins(client_socket, client_host);
            bool continuing = continuing_result.value_or(false);

            // 阶段 12：封装 HTTP/1 连接并投递到当前 IO 调度器处理
            if (continuing) {
                HttpConnImpl<SocketType> conn(std::move(client_socket));

                // 在当前调度器上处理连接
                if (!schedule_runtime_task(scheduler, m_handler(std::move(conn)))) {
                    HTTP_LOG_ERROR("[runtime] [schedule-fail]",
                                   "context=connection-handler");
                }
            }
        }

        co_return;
    }

    /**
     * @brief 根据文件描述符创建客户端 Socket
     * @param fd accept 获得的文件描述符
     * @return 成功返回 Socket 对象，失败返回 std::nullopt
     */
    virtual std::optional<SocketType> create_client_socket(GHandle fd) {
        if constexpr (std::is_same_v<SocketType, AsyncTcpSocket>) {
            return SocketType(fd);
        } else {
            // SslSocket 需要在派生类中实现
            return std::nullopt;
        }
    }

    bool start_plugins() {
        m_started_plugin_count = 0;
        for (auto& plugin : m_accept_plugins) {
            if (!plugin->start(m_runtime)) {
                HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error=start returned false");
                stop_started_plugins();
                return false;
            }
            ++m_started_plugin_count;
        }
        return true;
    }

    void stop_started_plugins() noexcept {
        while (m_started_plugin_count > 0) {
            --m_started_plugin_count;
            m_accept_plugins[m_started_plugin_count]->stop();
        }
    }

    Task<bool> run_accept_plugins(SocketType& client_socket, const Host& client_host) {
        for (auto& plugin : m_accept_plugins) {
            auto plugin_result = co_await plugin->handle(get_runtime(), client_socket, client_host);
            if (!plugin_result) {
                HTTP_LOG_ERROR("[accept-plugin] [task-fail]",
                               "error={}",
                               plugin_result.error().message());
                co_return false;
            }
            if (!plugin_result.value()) {
                co_return false;
            }
        }
        co_return true;
    }

    std::expected<AsyncTcpSocket, IOError> create_listener_socket() {
        auto listener = AsyncTcpSocket::create(IPType::IPV4);
        if (!listener) {
            return std::unexpected(listener.error());
        }
        if (auto result = listener->option().handle_reuse_addr(); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = listener->option().handle_reuse_port(); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = listener->option().handle_non_block(); !result) {
            return std::unexpected(result.error());
        }
        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        if (auto result = listener->bind(bind_host); !result) {
            return std::unexpected(result.error());
        }
        if (auto result = listener->listen(m_config.backlog); !result) {
            return std::unexpected(result.error());
        }
        return std::move(*listener);
    }

protected:
    Runtime m_runtime;                      ///< 内部 Runtime 实例
    HttpServerConfig m_config;              ///< 服务器配置
    HttpConnHandler m_handler;                  ///< 连接处理器
    std::vector<std::unique_ptr<plugin::AcceptPlugin<SocketType>>> m_accept_plugins; ///< accept 后顺序执行的插件列表
    std::size_t m_started_plugin_count = 0;  ///< 已成功启动且需要反序停止的插件数量
    std::optional<HttpRouter> m_router;     ///< 路由表（路由模式下使用）
    std::vector<AsyncTcpSocket> m_listeners;     ///< 每个 IO 调度器独立 listener，stop() 同步关闭
    std::atomic<bool> m_running;            ///< 运行状态标志
};

// 类型别名 - HTTP (AsyncTcpSocket)
using HttpConnHandler = HttpConnHandlerImpl<AsyncTcpSocket>;
using HttpServer = HttpServerImpl<AsyncTcpSocket>;
inline HttpServer HttpServerBuilder::build() const { return HttpServer(m_config); }

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief HTTPS 服务器配置
 * @details
 * - `cert_path` / `key_path` 是 TLS 服务端证书与私钥
 * - `ca_path`、`verify_peer`、`verify_depth` 用于双向 TLS 或客户端证书校验
 * - `tcp_no_delay` 控制 accept 后的 TLS 底层 TCP socket 是否启用 TCP_NODELAY
 * - `reader_setting` / `writer_setting` 仅在 TLS 连接路径上生效
 */
struct HttpsServerConfig
{
    std::string host = "0.0.0.0";              ///< 监听地址
    std::string cert_path;                      ///< TLS 服务端证书路径
    std::string key_path;                       ///< TLS 服务端私钥路径
    std::string ca_path;                        ///< CA 证书路径（用于客户端证书校验）
    HttpReaderSetting reader_setting;           ///< TLS 连接的读取器配置
    HttpWriterSetting writer_setting;           ///< TLS 连接的写入器配置
    RuntimeAffinityConfig affinity;             ///< 调度器绑核策略
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO; ///< IO 调度器数量
    size_t parallel_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO; ///< 计算调度器数量
    int backlog = 128;                          ///< listen backlog 队列长度
    int verify_depth = 4;                       ///< 证书链校验深度
    uint16_t port = 443;                        ///< 监听端口
    bool tcp_no_delay = true;                   ///< 是否为已接受连接启用 TCP_NODELAY
    bool verify_peer = false;                   ///< 是否校验客户端证书
};

class HttpsServer;

/**
 * @brief HTTPS 服务器 builder
 * @details 除监听配置外，还负责收集 TLS 上下文初始化所需的证书与验证策略。
 */
class HttpsServerBuilder {
public:
    HttpsServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; } ///< 设置监听地址
    HttpsServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; } ///< 设置监听端口
    HttpsServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; } ///< 设置 listen backlog
    HttpsServerBuilder& tcp_no_delay(bool v)               { m_config.tcp_no_delay = v; return *this; } ///< 设置已接受连接是否启用 TCP_NODELAY
    HttpsServerBuilder& io_scheduler_count(size_t v)       { m_config.io_scheduler_count = v; return *this; } ///< 设置 IO 调度器数量
    HttpsServerBuilder& parallel_scheduler_count(size_t v)  { m_config.parallel_scheduler_count = v; return *this; } ///< 设置计算调度器数量
    HttpsServerBuilder& sequential_affinity(size_t io_count, size_t parallel_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_parallel_count = parallel_count;
        return *this;
    }
    bool custom_affinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> parallel_cpus) {
        if (io_cpus.size() != m_config.io_scheduler_count ||
            parallel_cpus.size() != m_config.parallel_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(io_cpus);
        m_config.affinity.custom_parallel_cpus = std::move(parallel_cpus);
        return true;
    }
    HttpsServerBuilder& cert_path(std::string v)          { m_config.cert_path = std::move(v); return *this; } ///< 设置证书路径
    HttpsServerBuilder& key_path(std::string v)           { m_config.key_path = std::move(v); return *this; } ///< 设置私钥路径
    HttpsServerBuilder& ca_path(std::string v)            { m_config.ca_path = std::move(v); return *this; } ///< 设置 CA 证书路径
    HttpsServerBuilder& verify_peer(bool v)               { m_config.verify_peer = v; return *this; } ///< 设置是否校验客户端证书
    HttpsServerBuilder& verify_depth(int v)               { m_config.verify_depth = v; return *this; } ///< 设置证书链校验深度
    HttpsServer build() const; ///< 构建 HTTPS 服务器实例
    HttpsServerConfig build_config() const                { return m_config; } ///< 导出配置
private:
    HttpsServerConfig m_config;
};

using HttpsConnHandler = HttpConnHandlerImpl<galay::ssl::SslSocket>;

/**
 * @brief HTTPS服务器类
 * @details
 * 该类在 `start_internal()` 中初始化 TLS 上下文，然后复用 `HttpServerImpl` 的 runtime、
 * accept 循环与连接分发逻辑。证书加载失败或 TLS 上下文不可用时，启动会失败并返回 false。
 */
class HttpsServer : public HttpServerImpl<galay::ssl::SslSocket>
{
public:
    explicit HttpsServer(const HttpsServerConfig& config)
        : HttpServerImpl<galay::ssl::SslSocket>(convert_config(config))
        , m_https_config(config)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Server)
    {
    }

    ~HttpsServer() override = default;

protected:
    bool start_internal() override {
        // 初始化 SSL 上下文
        if (!init_ssl_context()) {
            return false;
        }

        return HttpServerImpl<galay::ssl::SslSocket>::start_internal();
    }

    std::optional<galay::ssl::SslSocket> create_client_socket(GHandle fd) override {
        if (!m_ssl_ctx.is_valid()) {
            return std::nullopt;
        }

        return galay::ssl::SslSocket(&m_ssl_ctx, fd);
    }

    Task<void> server_loop(IOScheduler* scheduler, AsyncTcpSocket* listener) override {
        if (listener == nullptr) {
            co_return;
        }

        // 阶段 7：主 accept 循环，运行期间持续等待新连接
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener->accept(&client_host);

            // 阶段 8：处理 accept 失败，服务器仍运行时记录告警并继续循环
            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_WARN("[accept] [fail]", "error={}", accept_result.error().message());
                }
                continue;
            }


            // 阶段 9：根据 accept 得到的句柄构造 SSL 客户端 socket
            auto client_socket_opt = create_client_socket(*accept_result);
            if (!client_socket_opt) {
                continue;
            }

            // 阶段 10：配置客户端 socket 的非阻塞与 TCP_NODELAY 选项
            galay::ssl::SslSocket client_socket = std::move(*client_socket_opt);
            auto nonblock_result = client_socket.option().handle_non_block();
            if (!nonblock_result) {
                continue;
            }
            if (m_config.tcp_no_delay) {
                auto nodelay_result = client_socket.option().handle_tcp_no_delay();
                if (!nodelay_result) {
                    HTTP_LOG_DEBUG("[socket] [nodelay]", "failed to set TCP_NODELAY");
                }
            }

            // 阶段 11：执行 accept plugin，允许用户在 TLS 握手前拦截 socket
            auto continuing_result = co_await this->run_accept_plugins(client_socket, client_host);
            bool continuing = continuing_result.value_or(false);
            if (!continuing) {
                auto close_result = co_await client_socket.close();
                if (!close_result) {
                    HTTP_LOG_WARN("[socket] [close-fail]",
                                  "context=ssl-accept-plugin-stop error={}",
                                  close_result.error().message());
                }
                continue;
            }

            // 阶段 12：选择连接处理调度器，默认回退到当前 IO 调度器
            auto* target_scheduler = m_runtime.get_next_io_scheduler();
            if (target_scheduler == nullptr) {
                target_scheduler = scheduler;
            }

            // 阶段 13：投递 TLS 连接处理任务，投递失败时关闭客户端 socket
            if (!this->schedule_runtime_task(target_scheduler, handle_ssl_connection(std::move(client_socket)))) {
                auto close_result = co_await client_socket.close();
                if (!close_result) {
                    HTTP_LOG_WARN("[socket] [close-fail]",
                                  "context=ssl-schedule-fail error={}",
                                  close_result.error().message());
                }
            }
        }

        co_return;
    }

private:
    Task<void> handle_ssl_connection(galay::ssl::SslSocket socket) {
        auto handshake_result = co_await socket.handshake();
        if (!handshake_result) {
            HTTP_LOG_WARN("[ssl] [handshake] [fail]", "error={}", handshake_result.error().message());
            auto close_result = co_await socket.close();
            if (!close_result) {
                HTTP_LOG_WARN("[socket] [close-fail]",
                              "context=ssl-handshake-fail error={}",
                              close_result.error().message());
            }
            co_return;
        }


        // 创建连接并调用处理器
        HttpConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        co_await m_handler(std::move(conn));
        co_return;
    }

    static HttpServerConfig convert_config(const HttpsServerConfig& config) {
        HttpServerConfig base_config;
        base_config.host = config.host;
        base_config.port = config.port;
        base_config.backlog = config.backlog;
        base_config.tcp_no_delay = config.tcp_no_delay;
        base_config.io_scheduler_count = config.io_scheduler_count;
        base_config.parallel_scheduler_count = config.parallel_scheduler_count;
        base_config.affinity = config.affinity;
        return base_config;
    }

    bool init_ssl_context() {
        if (!m_ssl_ctx.is_valid()) {
            return false;
        }

        // 加载证书
        if (!m_https_config.cert_path.empty()) {
            auto result = m_ssl_ctx.load_certificate(m_https_config.cert_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [cert] [fail]", "path={}", m_https_config.cert_path);
                return false;
            }
        }

        // 加载私钥
        if (!m_https_config.key_path.empty()) {
            auto result = m_ssl_ctx.load_private_key(m_https_config.key_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [key] [fail]", "path={}", m_https_config.key_path);
                return false;
            }
        }

        // 加载 CA 证书
        if (!m_https_config.ca_path.empty()) {
            auto result = m_ssl_ctx.load_ca_certificate(m_https_config.ca_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [ca] [fail]", "path={}", m_https_config.ca_path);
                return false;
            }
        }

        // 设置验证模式
        if (m_https_config.verify_peer) {
            m_ssl_ctx.set_verify_mode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.set_verify_depth(m_https_config.verify_depth);
        } else {
            m_ssl_ctx.set_verify_mode(galay::ssl::SslVerifyMode::None);
        }

        return true;
    }

    HttpsServerConfig m_https_config;
    galay::ssl::SslContext m_ssl_ctx;
};

inline HttpsServer HttpsServerBuilder::build() const { return HttpsServer(m_config); }
#endif

} // namespace galay::http

#endif // GALAY_HTTP_SERVER_H
