/**
 * @file http2_server.h
 * @brief HTTP/2 服务器，支持 h2c prior knowledge 和 TLS ALPN 协商
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 H2cServer 和 H2Server 模板类，支持两种 HTTP/2 服务模式：
 *          1. h2c 模式：通过 prior knowledge 直接使用 HTTP/2
 *          2. TLS 模式：通过 ALPN 协议协商直接使用 HTTP/2
 */

#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include "../kernel/http2_conn.h"
#include "../kernel/stream_manager.h"
#include "../kernel/http2_stream.h"
#include "h2_static_file.h"
#include "http2_router.h"
#include "../../galay-http/server/server_routes.h"
#include "../../galay-http/common/iovec_utils.h"
#include "../protoc/http2_base.h"
#include "../protoc/http2_frame.h"
#include "../../galay-http/protoc/http_header.h"
#include "../../galay-http/protoc/http_request.h"
#include "../../galay-http/common/http_log.h"
#include "../../galay-http/kernel/http_conn.h"
#include "../../galay-http/builder/http_builder.h"
#include "../../galay-http/plugin/common/defn.h"
#include "../../galay-http/server/server_listener.h"
#include "../../galay-kernel/async/async_tcp.h"
#include "../../galay-kernel/core/runtime.h"
#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/ssl/ssl_context.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif
#include <memory>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <expected>
#include <string>
#include <array>
#include <optional>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace galay::http2
{

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;

template<typename SocketType>
inline Task<void> run_default_http1_fallback_loop(const char* log_tag,
                                              galay::http::HttpConnImpl<SocketType>&& conn,
                                              galay::http::server_detail::ServerConnections::Scope& connection_scope) {
    bool keep_alive = true;
    while (keep_alive) {
        galay::http::HttpRequest request;
        auto reader = conn.get_reader();
        auto read_result = co_await reader.get_request(request);
        if (!read_result) {
            HTTP_LOG_DEBUG("[h1-fallback]",
                           "{} recv failed: {}",
                           log_tag,
                           read_result.error().message());
            break;
        }

        keep_alive = request.header().is_keep_alive() && !request.header().is_connection_close();

        auto response = galay::http::Http1_1ResponseBuilder()
            .status(galay::http::HttpStatusCode::NotFound_404)
            .header("Content-Type", "text/plain")
            .body("404 Not Found")
            .build_move();
        auto writer = conn.get_writer();
        auto write_result = co_await writer.send_response(response);
        if (!write_result) {
            HTTP_LOG_DEBUG("[h1-fallback]",
                           "{} send failed: {}",
                           log_tag,
                           write_result.error().message());
            break;
        }
    }
    connection_scope.release_handle();
    auto close_result = co_await conn.close();
    if (!close_result) {
        HTTP_LOG_WARN("[h1-fallback] [close-fail]",
                      "{} error={}",
                      log_tag,
                      close_result.error().message());
    }
    co_return;
}

/**
 * @brief HTTP/2 流处理器类型（每个新流创建后 spawn handler(stream)）
 */
using Http2ConnectionHandler = Http2StreamHandler;

/**
 * @brief h2c 服务器配置
 */
struct H2cServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    bool tcp_no_delay = true;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t parallel_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;

    // HTTP/2 设置
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;  // 默认禁用 Server Push（curl 不支持）

    // 连接运行时策略
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
    Http2ConnectionHandler stream_handler;
    Http2ActiveConnHandler active_conn_handler;
    std::vector<H2StaticRoute> static_routes;
    std::vector<H2StaticFileMount> static_file_mounts;
};

class H2cServer;

template<bool EnableSwagger = false>
class H2cServerBuilder : public galay::http::server_detail::ServerRoutes<EnableSwagger> {
public:
    H2cServerBuilder() = default;
    explicit H2cServerBuilder(H2cServerConfig config) : m_config(std::move(config)) {}
    H2cServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2cServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2cServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2cServerBuilder& tcp_no_delay(bool v)               { m_config.tcp_no_delay = v; return *this; }
    H2cServerBuilder& io_scheduler_count(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2cServerBuilder& parallel_scheduler_count(size_t v)  { m_config.parallel_scheduler_count = v; return *this; }
    H2cServerBuilder& max_concurrent_streams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cServerBuilder& initial_window_size(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cServerBuilder& max_frame_size(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cServerBuilder& max_header_list_size(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cServerBuilder& enable_push(bool v)               { m_config.enable_push = v; return *this; }
    H2cServerBuilder& ping_enabled(bool v)              { m_config.ping_enabled = v; return *this; }
    H2cServerBuilder& ping_interval(std::chrono::milliseconds v) { m_config.ping_interval = v; return *this; }
    H2cServerBuilder& ping_timeout(std::chrono::milliseconds v) { m_config.ping_timeout = v; return *this; }
    H2cServerBuilder& settings_ack_timeout(std::chrono::milliseconds v) { m_config.settings_ack_timeout = v; return *this; }
    H2cServerBuilder& graceful_shutdown_rtt(std::chrono::milliseconds v) { m_config.graceful_shutdown_rtt = v; return *this; }
    H2cServerBuilder& graceful_shutdown_timeout(std::chrono::milliseconds v) { m_config.graceful_shutdown_timeout = v; return *this; }
    H2cServerBuilder& flow_control_target_window(uint32_t v) { m_config.flow_control_target_window = v; return *this; }
    H2cServerBuilder& flow_control_strategy(Http2FlowControlStrategy v) {
        m_config.flow_control_strategy = std::move(v);
        return *this;
    }
    H2cServerBuilder& stream_handler(Http2ConnectionHandler handler) {
        m_config.stream_handler = std::move(handler);
        return *this;
    }
    H2cServerBuilder& active_conn_handler(Http2ActiveConnHandler handler) {
        m_config.active_conn_handler = std::move(handler);
        return *this;
    }
    /**
     * @brief 注册 HTTP/2 exact path 静态响应配置。
     * @param path 需要与 request `:path` 精确匹配的路径。
     * @param response 由 builder 移动保存的静态响应配置。
     * @return 当前 builder，支持链式调用。
     * @note 该接口只配置路由，不启动阻塞 I/O，也不改变 stream_handler/active_conn_handler API。
     */
    H2cServerBuilder& static_response(std::string path, H2StaticResponse response) {
        m_config.static_routes.push_back(make_h2_static_route(std::move(path), std::move(response)));
        return *this;
    }
    /**
     * @brief 注册 HTTP/2 静态文件挂载点。
     * @param prefix request `:path` 前缀，例如 `/assets`。
     * @param config 静态文件根目录、缓存阈值和 ETag 配置。
     * @return 当前 builder，支持链式调用。
     * @note 该入口只启用 HTTP/2 DATA frame 用户态发送；h2 TLS 不使用 kernel sendfile。
     */
    H2cServerBuilder& static_files(std::string prefix, H2StaticFileConfig config) {
        m_config.static_file_mounts.push_back(
            make_h2_static_file_mount(std::move(prefix), std::move(config)));
        return *this;
    }
    H2cServerBuilder& sequential_affinity(size_t io_count, size_t parallel_count) {
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
    api::ApiResult<std::unique_ptr<H2cServer>> build();
    H2cServerConfig build_config() const {
        return Http2Conn::normalize_settings_config(m_config);
    }
private:
    H2cServerConfig m_config;
};

/**
 * @brief 协议检测结果
 */
enum class DetectedProtocol {
    Unknown,            // 检测失败
    H2cPriorKnowledge,  // 直接 h2c
    H2cUpgrade,         // HTTP/1.1 Upgrade: h2c
    Http1,              // 普通 HTTP/1.1（降级）
};

/**
 * @brief 判断首字节是否像 HTTP method（大写 ASCII 字母）
 */
inline bool looks_like_http_method(const char* buf) {
    return buf[0] >= 'A' && buf[0] <= 'Z';
}

/**
 * @brief 从 RingBuffer 的 iovec 拷贝 n 字节到 buf（不 consume）
 */
inline void peek_ring_buffer(RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent>& rb, char* buf, size_t n) {
    auto iovecs = borrow_read_iovecs(rb);
    size_t copied = 0;
    for (const auto& iov : iovecs) {
        size_t to_copy = std::min(iov.iov_len, n - copied);
        std::memcpy(buf + copied, iov.iov_base, to_copy);
        copied += to_copy;
        if (copied >= n) break;
    }
}

/**
 * @brief 把 RingBuffer 全部数据取出到 string 并 consume
 */
inline std::string drain_ring_buffer(RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent>& rb) {
    std::string data;
    data.reserve(rb.readable());
    auto iovecs = borrow_read_iovecs(rb);
    for (const auto& iov : iovecs) {
        data.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
    }
    rb.consume(rb.readable());
    return data;
}

/**
 * @brief HTTP/1.1 降级处理器类型
 */
using Http1FallbackHandler = std::function<Task<void>(
    galay::http::HttpConnImpl<AsyncTcpSocket>, galay::http::HttpRequestHeader)>;

/**
 * @brief h2c 服务器 (HTTP/2 over cleartext)
 */
class H2cServer
{
public:
    explicit H2cServer(const H2cServerConfig& config = H2cServerConfig())
        : m_runtime(RuntimeBuilder().io_scheduler_count(config.io_scheduler_count)
                                   .parallel_scheduler_count(config.parallel_scheduler_count)
                                   .apply_affinity(config.affinity)
                                   .build())
        , m_config(config)
        , m_stream_handler(config.stream_handler)
        , m_active_conn_handler(config.active_conn_handler)
        , m_running(false)
    {
    }
    
    ~H2cServer() {
        stop();
    }
    
    H2cServer(const H2cServer&) = delete;
    H2cServer& operator=(const H2cServer&) = delete;

    H2cServer(const H2cServerConfig& config, galay::http::HttpRouter&& router) : H2cServer(config)
    {
        m_stream_handler = [routes = std::make_shared<galay::http::HttpRouter>(std::move(router))](Http2Stream::ptr stream) {
            return server_detail::execute_http2_route(routes, std::move(stream));
        };
    }

    api::ApiResult<void> start() {
        if (m_running.load()) return std::unexpected(api::ApiError{api::ApiErrorCode::kServerError,
            "server is already running", 409});
        if (m_start_attempted) return std::unexpected(api::ApiError{api::ApiErrorCode::kServerError,
            "server has already attempted to start", 409});
        m_start_attempted = true;
        if (!start_internal()) {
            stop();
            return std::unexpected(api::ApiError{api::ApiErrorCode::kTransportError, m_start_error, 500});
        }
        return {};
    }

    void start(Http2ConnectionHandler handler) {
        if (m_running.load()) return;
        m_stream_handler = std::move(handler);
        m_active_conn_handler = nullptr;
        if (!start_internal()) HTTP_LOG_ERROR("[h2c] [start-fail]", "error={}", m_start_error);
    }

    void start(Http2ActiveConnHandler handler) {
        if (m_running.load()) return;
        m_active_conn_handler = std::move(handler);
        if (!start_internal()) HTTP_LOG_ERROR("[h2c] [start-fail]", "error={}", m_start_error);
    }

    void set_http1_fallback(Http1FallbackHandler handler) {
        m_http1_fallback = std::move(handler);
    }
    
    void stop() {
        if (!m_running.load()) {
            stop_started_plugins();
            m_listeners.clear();
            return;
        }

        m_running.store(false);
        HTTP_LOG_INFO("[h2c] [server] [stopping]", "port={}", m_config.port);

        stop_started_plugins();
        galay::http::server_detail::close_listeners(m_runtime, m_listeners);
        m_connections.stop(m_runtime);
        m_runtime.stop();
        m_listeners.clear();
        HTTP_LOG_INFO("[h2c] [server] [stopped]", "port={}", m_config.port);
    }
    
    bool is_running() const {
        return m_running.load();
    }

    const std::string& start_error() const noexcept { return m_start_error; }

    /**
     * @brief 检查至少一个 h2c listener 是否已完成 bind/listen。
     * @return 服务器正在运行且已有 listener 可接受连接时返回 true。
     * @note 该查询无锁、不阻塞，供启动编排和测试等待可观测就绪状态。
     */
    bool is_ready() const {
        return m_running.load(std::memory_order_acquire) &&
               m_listening_loop_count.load(std::memory_order_acquire) > 0;
    }
    
    Runtime& get_runtime() {
        return m_runtime;
    }

    /**
     * @brief 注册 h2c accept 后、HTTP/2 协议处理前执行的插件。
     * @param plugin 由 server 接管生命周期的插件实例。
     * @return 注册成功返回 true；服务器已启动或 plugin 为空时返回 false。
     * @details
     * - 必须在 start(...) 前调用，启动后不允许修改插件列表。
     * - `start()` 在 runtime 启动后、accept loop 投递前按注册顺序调用。
     * - `stop()` 在 runtime 停止前按注册反序调用。
     * - `handle()` 返回 false 时停止后续插件，并跳过当前连接的 HTTP/2 处理。
     */
    bool add_accept_plugin(std::unique_ptr<galay::http::plugin::AcceptPlugin<AsyncTcpSocket>> plugin) {
        if (m_running.load() || !plugin) {
            return false;
        }

        m_accept_plugins.push_back(std::move(plugin));
        return true;
    }

private:
    /**
     * @brief 将 server 拥有的 root task 绑定到当前 Runtime 后提交到指定调度器。
     * @details HTTP/2 静态文件异步读取依赖 `RuntimeHandle::current()` 派生
     *          blocking task；server 直接使用裸 scheduler 投递 root task 时必须显式绑定。
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

    bool start_internal() {
        if (m_running.load()) {
            m_start_error = "server is already running";
            HTTP_LOG_WARN("[h2c] [server]", "already running");
            return false;
        }
        m_start_error.clear();
        if (!m_stream_handler && !m_active_conn_handler) {
            m_start_error = "missing HTTP/2 stream handler";
            HTTP_LOG_ERROR("[h2c] [handler]", "missing");
            return false;
        }

        auto runtime_start = m_runtime.start();
        if (!runtime_start.has_value()) {
            m_start_error = "runtime: " + std::string(runtime_start.error().message());
            HTTP_LOG_ERROR("[h2c] [runtime-start-fail]",
                           "error={}",
                           runtime_start.error().message());
            return false;
        }

        m_connections.start(m_runtime);

        if (!start_plugins()) {
            m_start_error = "accept plugin failed to start";
            m_connections.stop(m_runtime);
            m_runtime.stop();
            return false;
        }

        const size_t io_scheduler_count = m_runtime.get_io_scheduler_count();
        if (io_scheduler_count == 0) {
            m_start_error = "server requires at least one IO scheduler";
            stop_started_plugins();
            m_connections.stop(m_runtime);
            m_runtime.stop();
            return false;
        }
        m_listeners.reserve(io_scheduler_count);
        for (size_t i = 0; i < io_scheduler_count; ++i) {
            auto listener = galay::http::server_detail::create_listener(m_config.host, m_config.port, m_config.backlog);
            if (!listener) {
                m_start_error = "listen " + m_config.host + ":" + std::to_string(m_config.port) + ": " + listener.error().message();
                stop_started_plugins();
                m_connections.stop(m_runtime);
                m_runtime.stop();
                m_listeners.clear();
                return false;
            }
            m_listeners.push_back(std::move(*listener));
        }
        m_running.store(true);
        HTTP_LOG_INFO("[server] [listen] [h2c]",
                      "host={} port={}",
                      m_config.host,
                      m_config.port);

        // Spawn one server_loop per IO scheduler with SO_REUSEPORT
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.get_io_scheduler(i);
            if (scheduler) {
                auto loop = server_loop(scheduler, &m_listeners[i]);
                if (!schedule_runtime_task(scheduler, std::move(loop))) {
                    m_start_error = "failed to schedule server loop " + std::to_string(i);
                    HTTP_LOG_ERROR("[h2c] [schedule-fail]", "server-loop");
                    m_running.store(false);
                    stop_started_plugins();
                    galay::http::server_detail::close_listeners(m_runtime, m_listeners);
                    m_connections.stop(m_runtime);
                    m_runtime.stop();
                    m_listeners.clear();
                    return false;
                }
            }
        }

        return true;
    }

    Task<void> server_loop(IOScheduler* scheduler, AsyncTcpSocket* listener_socket) {
        if (!listener_socket) co_return;
        auto& listener = *listener_socket;
        // 阶段 1：注册 server_loop 退出守卫，确保循环结束时扣减运行计数
        struct LoopExitGuard {
            H2cServer* server;
            ~LoopExitGuard() {
                server->m_listening_loop_count.fetch_sub(1, std::memory_order_acq_rel);
            }
        } guard{this};
        m_listening_loop_count.fetch_add(1, std::memory_order_release);

        // 阶段 8：主 accept 循环，运行期间持续等待新连接
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);

            // 阶段 9：处理 accept 失败，服务器仍运行时记录错误并继续循环
            if (!accept_result) {
                if (m_running.load()) {
                    HTTP_LOG_ERROR("[accept] [fail]",
                                   "error={}",
                                   accept_result.error().message());
                }
                continue;
            }

            HTTP_LOG_INFO("[connect] [h2c]",
                          "ip={} port={}",
                          client_host.ip(),
                          client_host.port());

            // 阶段 10：根据 accept 得到的句柄构造 TCP 客户端 socket
            AsyncTcpSocket client_socket(*accept_result);
            // 阶段 11：配置客户端 socket 为非阻塞模式
            auto nonblock_result = client_socket.option().handle_non_block();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [client]",
                               "error={}",
                               nonblock_result.error().message());
                continue;
            }
            if (m_config.tcp_no_delay) {
                auto nodelay_result = client_socket.option().handle_tcp_no_delay();
                if (!nodelay_result) {
                    HTTP_LOG_DEBUG("[socket] [nodelay]", "failed to set TCP_NODELAY");
                }
            }

            // 阶段 12：执行 accept plugin，允许插件在协议检测前拦截连接
            auto continuing_result = co_await run_accept_plugins(client_socket, client_host);
            bool continuing = continuing_result.value_or(false);
            if (!continuing) {
                auto close_result = co_await client_socket.close();
                if (!close_result) {
                    HTTP_LOG_WARN("[socket] [close-fail] [client]",
                                  "error={}",
                                  close_result.error().message());
                }
                continue;
            }

            // 阶段 13：把 h2c 连接处理任务轮询分发到 IO 调度器，避免 loopback
            // SO_REUSEPORT 哈希倾斜时所有连接集中在单个 accept scheduler。
            auto* target_scheduler = m_runtime.get_next_io_scheduler();
            if (!schedule_runtime_task(target_scheduler, handle_connection(std::move(client_socket)))) {
                HTTP_LOG_ERROR("[h2c] [schedule-fail]", "handle-connection");
                auto close_result = co_await client_socket.close();
                if (!close_result) {
                    HTTP_LOG_WARN("[socket] [close-fail] [client]",
                                  "error={}",
                                  close_result.error().message());
                }
            }
        }

        co_return;
    }
    
    /**
     * @brief 处理新连接
     */
    Task<void> handle_connection(AsyncTcpSocket socket) {
        auto tracked = co_await m_connections.attach(socket.handle().fd);
        if (!tracked) {
            HTTP_LOG_WARN("[h2c] [connection-track-fail]", "error={}", tracked.error().message());
            auto close_result = co_await socket.close();
            if (!close_result && close_result.error().code() != kClosed) {
                HTTP_LOG_WARN("[socket] [close-fail]", "context=h2c-track-fail error={}", close_result.error().message());
            }
            co_return;
        }
        auto connection_scope = std::move(*tracked);
        Http2ConnImpl<AsyncTcpSocket> conn(std::move(socket));

        // 配置本地设置
        auto local_settings = Http2Conn::make_settings_frame_from_config(m_config);
        if (conn.apply_local_settings(local_settings) != Http2ErrorCode::NoError) {
            connection_scope.release_handle();
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_WARN("[h2c] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            co_return;
        }
        conn.runtime_config().from(m_config);

        DetectedProtocol protocol = DetectedProtocol::Unknown;
        galay::http::HttpRequestHeader upgrade_request;
        co_await detect_protocol(conn, protocol, upgrade_request);

        switch (protocol) {
        case DetectedProtocol::H2cPriorKnowledge:
        case DetectedProtocol::H2cUpgrade: {
            if (protocol == DetectedProtocol::H2cUpgrade) {
                auto decoded = Http2Conn::decode_h2c_upgrade_settings_header(
                    upgrade_request.header_pairs().get_value("HTTP2-Settings"));
                if (!decoded.has_value() ||
                    conn.apply_peer_settings(*decoded) != Http2ErrorCode::NoError) {
                    connection_scope.release_handle();
                    auto close_result = co_await conn.close();
                    if (!close_result) {
                        HTTP_LOG_WARN("[h2c] [close-fail]",
                                      "error={}",
                                      close_result.error().message());
                    }
                    co_return;
                }
            }
            // 初始化 StreamManager 并启动帧分发循环
            conn.init_stream_manager();
            auto* mgr = conn.stream_manager();
            HTTP_LOG_DEBUG("[h2] [stream-mgr]", "starting");
            if (m_active_conn_handler) {
                co_await mgr->start(m_active_conn_handler);
            } else {
                co_await mgr->start(m_stream_handler);
            }
            HTTP_LOG_DEBUG("[h2] [stream-mgr]", "stopped");
            connection_scope.release_handle();
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_WARN("[h2c] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            break;
        }
        case DetectedProtocol::Http1:
            co_await handle_http1_fallback(std::move(conn), std::move(upgrade_request), connection_scope);
            break;
        default:
            HTTP_LOG_ERROR("[protocol] [detect-fail]", "h2c unknown");
            connection_scope.release_handle();
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_WARN("[h2c] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            break;
        }

        co_return;
    }

    Task<void> read_at_least(Http2ConnImpl<AsyncTcpSocket>& conn, size_t n) {
        auto& rb = conn.ring_buffer();
        while (rb.readable() < n) {
            auto write_iovecs = borrow_write_iovecs(rb);
            auto result = co_await conn.socket().readv(write_iovecs.storage(), write_iovecs.size());
            if (!result || result.value() == 0) {
                co_return;
            }
            rb.produce(result.value());
        }
        co_return;
    }

    /**
     * @brief 检测协议类型并完成初始握手
     * @param conn HTTP/2 连接
     * @param protocol 输出协议类型
     * @param upgrade_request 输出首个 HTTP/1.1 请求头（Upgrade/Http1 路径）
     */
    Task<void> detect_protocol(Http2ConnImpl<AsyncTcpSocket>& conn,
                              DetectedProtocol& protocol,
                              galay::http::HttpRequestHeader& upgrade_request) {
        protocol = DetectedProtocol::Unknown;
        auto& rb = conn.ring_buffer();

        co_await read_at_least(conn, kHttp2ConnectionPrefaceLength);
        if (rb.readable() < kHttp2ConnectionPrefaceLength) {
            co_return;
        }

        char peek_buf[kHttp2ConnectionPrefaceLength];
        peek_ring_buffer(rb, peek_buf, kHttp2ConnectionPrefaceLength);

        // ===== Prior Knowledge =====
        if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) == 0) {
            HTTP_LOG_DEBUG("[h2] [prior-knowledge]", "detected");
            rb.consume(kHttp2ConnectionPrefaceLength);

            auto settings_result = co_await conn.send_settings();
            if (!settings_result) {
                co_return;
            }

            protocol = DetectedProtocol::H2cPriorKnowledge;
            co_return;
        }

        // ===== HTTP/1.1 (Upgrade or fallback) =====
        if (looks_like_http_method(peek_buf)) {
            HTTP_LOG_DEBUG("[h1] [detect]", "h2c fallback or upgrade");

            std::string header_data = drain_ring_buffer(rb);
            while (header_data.find("\r\n\r\n") == std::string::npos && header_data.size() < 8192) {
                auto write_iovecs = borrow_write_iovecs(rb);
                auto result = co_await conn.socket().readv(write_iovecs.storage(), write_iovecs.size());
                if (!result || result.value() == 0) {
                    co_return;
                }
                rb.produce(result.value());
                header_data.append(drain_ring_buffer(rb));
            }

            size_t header_end = header_data.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                HTTP_LOG_ERROR("[header] [invalid]", "too large");
                co_return;
            }

            auto parse_result = upgrade_request.from_string(
                std::string_view(header_data.data(), header_end + 4));
            if (parse_result.first != galay::http::kNoError || parse_result.second <= 0) {
                HTTP_LOG_ERROR("[header] [parse-fail]",
                               "code={}",
                               static_cast<int>(parse_result.first));
                co_return;
            }

            auto& headers = upgrade_request.header_pairs();
            std::string upgrade_value = headers.get_value("Upgrade");
            std::transform(upgrade_value.begin(), upgrade_value.end(), upgrade_value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            bool has_upgrade = (upgrade_value == "h2c");
            bool has_http2_settings = headers.has_key("HTTP2-Settings");

            if (has_upgrade && has_http2_settings) {
                HTTP_LOG_DEBUG("[h1] [upgrade] [h2c]", "detected");

                static constexpr char kUpgradeResp[] =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: h2c\r\n"
                    "\r\n";
                static constexpr size_t kUpgradeRespLen = sizeof(kUpgradeResp) - 1;

                size_t sent = 0;
                while (sent < kUpgradeRespLen) {
                    auto send_result = co_await conn.socket().send(
                        kUpgradeResp + sent, kUpgradeRespLen - sent);
                    if (!send_result) {
                        HTTP_LOG_ERROR("[upgrade] [send-fail]",
                                       "error={}",
                                       send_result.error().message());
                        co_return;
                    }
                    sent += send_result.value();
                }
                HTTP_LOG_DEBUG("[upgrade] [101-sent]", "h2c");

                // HTTP 头后面可能已带部分 Connection Preface，写入 RingBuffer
                if (header_data.size() > header_end + 4) {
                    const size_t remaining = header_data.size() - header_end - 4;
                    const size_t written = rb.try_write_batch(
                        header_data.data() + header_end + 4, remaining);
                    if (written != remaining) {
                        HTTP_LOG_ERROR("[upgrade] [buffer-full]", "h2c preface");
                        co_return;
                    }
                }

                co_await read_at_least(conn, kHttp2ConnectionPrefaceLength);
                if (rb.readable() < kHttp2ConnectionPrefaceLength) {
                    HTTP_LOG_ERROR("[preface] [recv-fail]", "h2c");
                    co_return;
                }

                peek_ring_buffer(rb, peek_buf, kHttp2ConnectionPrefaceLength);
                if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
                    HTTP_LOG_ERROR("[preface] [invalid]", "after upgrade");
                    co_return;
                }
                HTTP_LOG_DEBUG("[preface] [ok]", "h2c");

                rb.consume(kHttp2ConnectionPrefaceLength);

                auto settings_result = co_await conn.send_settings();
                if (!settings_result) {
                    co_return;
                }

                protocol = DetectedProtocol::H2cUpgrade;
                co_return;
            }

            // 回退到 HTTP/1.1 链路时，需要把已经读出的首个请求头（和可能携带的 body）
            // 回灌到 RingBuffer，交给标准 HttpReader 继续解析。
            if (!header_data.empty() && !m_http1_fallback) {
                const size_t written =
                    rb.try_write_batch(header_data.data(), header_data.size());
                if (written != header_data.size()) {
                    HTTP_LOG_ERROR("[protocol] [buffer-full]", "http1 fallback");
                    co_return;
                }
            }
            protocol = DetectedProtocol::Http1;
            co_return;
        }

        HTTP_LOG_WARN("[protocol] [unknown]", "h2c");
        co_return;
    }

    Task<void> handle_http1_fallback(Http2ConnImpl<AsyncTcpSocket>&& h2_conn,
                                   galay::http::HttpRequestHeader first_request_header,
                                   galay::http::server_detail::ServerConnections::Scope& connection_scope) {
        galay::http::HttpConnImpl<AsyncTcpSocket> conn(
            std::move(h2_conn.socket()), std::move(h2_conn.ring_buffer()));

        if (m_http1_fallback) {
            // Custom fallback owns connection lifetime, including protocol upgrades.
            connection_scope.finish();
            co_await m_http1_fallback(std::move(conn), std::move(first_request_header));
            co_return;
        }

        // 默认行为：进入 HTTP/1.1 处理链路，而不是直接返回 505。
        co_await run_default_http1_fallback_loop("[h2c] [h1-fallback]", std::move(conn), connection_scope);
        co_return;
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

    Task<bool> run_accept_plugins(AsyncTcpSocket& client_socket, const Host& client_host) {
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


private:
    Runtime m_runtime;
    H2cServerConfig m_config;
    Http2ConnectionHandler m_stream_handler;
    Http2ActiveConnHandler m_active_conn_handler;
    Http1FallbackHandler m_http1_fallback;
    std::vector<std::unique_ptr<galay::http::plugin::AcceptPlugin<AsyncTcpSocket>>> m_accept_plugins;
    std::size_t m_started_plugin_count = 0;
    std::atomic<bool> m_running;
    std::atomic<size_t> m_listening_loop_count{0};
    std::vector<AsyncTcpSocket> m_listeners;
    galay::http::server_detail::ServerConnections m_connections;
    std::string m_start_error;
    bool m_start_attempted = false;
};

template<bool EnableSwagger>
inline api::ApiResult<std::unique_ptr<H2cServer>> H2cServerBuilder<EnableSwagger>::build() {
    return this->template build_native<H2cServer>(build_config(), false);
}

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief h2 服务器配置（HTTP/2 over TLS）
 */
struct H2ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 9443;
    int backlog = 128;
    bool tcp_no_delay = true;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t parallel_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    RuntimeAffinityConfig affinity;

    // SSL 配置
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = false;
    int verify_depth = 4;

    // HTTP/2 设置
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool enable_push = false;

    // 连接运行时策略
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
    Http2ConnectionHandler stream_handler;
    Http2ActiveConnHandler active_conn_handler;
    std::vector<H2StaticRoute> static_routes;
    std::vector<H2StaticFileMount> static_file_mounts;
};

class H2Server;

template<bool EnableSwagger = false>
class H2ServerBuilder : public galay::http::server_detail::ServerRoutes<EnableSwagger> {
public:
    H2ServerBuilder() = default;
    explicit H2ServerBuilder(H2ServerConfig config) : m_config(std::move(config)) {}
    H2ServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2ServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2ServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2ServerBuilder& tcp_no_delay(bool v)               { m_config.tcp_no_delay = v; return *this; }
    H2ServerBuilder& io_scheduler_count(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2ServerBuilder& parallel_scheduler_count(size_t v)  { m_config.parallel_scheduler_count = v; return *this; }
    H2ServerBuilder& sequential_affinity(size_t io_count, size_t parallel_count) {
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
    H2ServerBuilder& cert_path(std::string v)          { m_config.cert_path = std::move(v); return *this; }
    H2ServerBuilder& key_path(std::string v)           { m_config.key_path = std::move(v); return *this; }
    H2ServerBuilder& ca_path(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    H2ServerBuilder& verify_peer(bool v)               { m_config.verify_peer = v; return *this; }
    H2ServerBuilder& verify_depth(int v)               { m_config.verify_depth = v; return *this; }
    H2ServerBuilder& max_concurrent_streams(uint32_t v) { m_config.max_concurrent_streams = v; return *this; }
    H2ServerBuilder& initial_window_size(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2ServerBuilder& max_frame_size(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2ServerBuilder& max_header_list_size(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2ServerBuilder& enable_push(bool v)               { m_config.enable_push = v; return *this; }
    H2ServerBuilder& ping_enabled(bool v)              { m_config.ping_enabled = v; return *this; }
    H2ServerBuilder& ping_interval(std::chrono::milliseconds v) { m_config.ping_interval = v; return *this; }
    H2ServerBuilder& ping_timeout(std::chrono::milliseconds v) { m_config.ping_timeout = v; return *this; }
    H2ServerBuilder& settings_ack_timeout(std::chrono::milliseconds v) { m_config.settings_ack_timeout = v; return *this; }
    H2ServerBuilder& graceful_shutdown_rtt(std::chrono::milliseconds v) { m_config.graceful_shutdown_rtt = v; return *this; }
    H2ServerBuilder& graceful_shutdown_timeout(std::chrono::milliseconds v) { m_config.graceful_shutdown_timeout = v; return *this; }
    H2ServerBuilder& flow_control_target_window(uint32_t v) { m_config.flow_control_target_window = v; return *this; }
    H2ServerBuilder& flow_control_strategy(Http2FlowControlStrategy v) {
        m_config.flow_control_strategy = std::move(v);
        return *this;
    }
    H2ServerBuilder& stream_handler(Http2ConnectionHandler handler) {
        m_config.stream_handler = std::move(handler);
        return *this;
    }
    H2ServerBuilder& active_conn_handler(Http2ActiveConnHandler handler) {
        m_config.active_conn_handler = std::move(handler);
        return *this;
    }
    /**
     * @brief 注册 HTTP/2 TLS exact path 静态响应配置。
     * @param path 需要与 request `:path` 精确匹配的路径。
     * @param response 由 builder 移动保存的静态响应配置。
     * @return 当前 builder，支持链式调用。
     * @note TLS 路径始终经用户态加密；该配置入口不启用 kernel sendfile。
     */
    H2ServerBuilder& static_response(std::string path, H2StaticResponse response) {
        m_config.static_routes.push_back(make_h2_static_route(std::move(path), std::move(response)));
        return *this;
    }
    /**
     * @brief 注册 HTTP/2 TLS 静态文件挂载点。
     * @param prefix request `:path` 前缀，例如 `/assets`。
     * @param config 静态文件根目录、缓存阈值和 ETag 配置。
     * @return 当前 builder，支持链式调用。
     * @note TLS 路径始终经用户态加密发送 DATA frame，不启用 kernel sendfile。
     */
    H2ServerBuilder& static_files(std::string prefix, H2StaticFileConfig config) {
        m_config.static_file_mounts.push_back(
            make_h2_static_file_mount(std::move(prefix), std::move(config)));
        return *this;
    }
    api::ApiResult<std::unique_ptr<H2Server>> build();
    H2ServerConfig build_config() const {
        return Http2Conn::normalize_settings_config(m_config);
    }
private:
    H2ServerConfig m_config;
};

/**
 * @brief h2 服务器 (HTTP/2 over TLS)
 */
class H2Server
{
public:
    explicit H2Server(const H2ServerConfig& config = H2ServerConfig())
        : m_runtime(RuntimeBuilder().io_scheduler_count(config.io_scheduler_count)
                                   .parallel_scheduler_count(config.parallel_scheduler_count)
                                   .apply_affinity(config.affinity)
                                   .build())
        , m_config(config)
        , m_stream_handler(config.stream_handler)
        , m_active_conn_handler(config.active_conn_handler)
        , m_running(false)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Server)
    {
    }

    ~H2Server() {
        stop();
    }

    H2Server(const H2Server&) = delete;
    H2Server& operator=(const H2Server&) = delete;

    H2Server(const H2ServerConfig& config, galay::http::HttpRouter&& router) : H2Server(config)
    {
        m_stream_handler = [routes = std::make_shared<galay::http::HttpRouter>(std::move(router))](Http2Stream::ptr stream) {
            return server_detail::execute_http2_route(routes, std::move(stream));
        };
    }

    api::ApiResult<void> start() {
        if (m_running.load()) return std::unexpected(api::ApiError{api::ApiErrorCode::kServerError,
            "server is already running", 409});
        if (m_start_attempted) return std::unexpected(api::ApiError{api::ApiErrorCode::kServerError,
            "server has already attempted to start", 409});
        m_start_attempted = true;
        if (!start_internal()) {
            stop();
            return std::unexpected(api::ApiError{api::ApiErrorCode::kTransportError, m_start_error, 500});
        }
        return {};
    }

    void start(Http2ConnectionHandler handler) {
        if (m_running.load()) return;
        m_stream_handler = std::move(handler);
        m_active_conn_handler = nullptr;
        if (!start_internal()) HTTP_LOG_ERROR("[h2] [start-fail]", "error={}", m_start_error);
    }

    void start(Http2ActiveConnHandler handler) {
        if (m_running.load()) return;
        m_active_conn_handler = std::move(handler);
        if (!start_internal()) HTTP_LOG_ERROR("[h2] [start-fail]", "error={}", m_start_error);
    }

    void set_http1_fallback(
        std::function<Task<void>(galay::http::HttpConnImpl<galay::ssl::SslSocket>)> handler) {
        m_http1_fallback = std::move(handler);
    }

    void stop() {
        if (!m_running.load()) {
            stop_started_plugins();
            m_listeners.clear();
            return;
        }

        m_running.store(false);
        stop_started_plugins();
        galay::http::server_detail::close_listeners(m_runtime, m_listeners);
        m_connections.stop(m_runtime);
        m_runtime.stop();
        m_listeners.clear();
    }

    bool is_running() const {
        return m_running.load();
    }

    const std::string& start_error() const noexcept { return m_start_error; }

    bool is_ready() const {
        return m_running.load(std::memory_order_acquire) &&
               m_listening_loop_count.load(std::memory_order_acquire) > 0;
    }

    Runtime& get_runtime() {
        return m_runtime;
    }

    /**
     * @brief 注册 h2 accept 后、TLS 握手前执行的插件。
     * @param plugin 由 server 接管生命周期的插件实例。
     * @return 注册成功返回 true；服务器已启动或 plugin 为空时返回 false。
     * @details
     * - 必须在 start(...) 前调用，启动后不允许修改插件列表。
     * - `start()` 在 runtime 启动后、accept loop 投递前按注册顺序调用。
     * - `stop()` 在 runtime 停止前按注册反序调用。
     * - `handle()` 返回 false 时停止后续插件，并跳过当前连接的 TLS/HTTP/2 处理。
     */
    bool add_accept_plugin(std::unique_ptr<galay::http::plugin::AcceptPlugin<galay::ssl::SslSocket>> plugin) {
        if (m_running.load() || !plugin) {
            return false;
        }

        m_accept_plugins.push_back(std::move(plugin));
        return true;
    }

private:
    static constexpr uint64_t kLowLatencyIoTimerTickNs = 1000000ULL;

    /**
     * @brief 将 TLS server 拥有的 root task 绑定到当前 Runtime 后提交。
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

    void configure_low_latency_io_timers() {
        for (size_t i = 0; i < m_runtime.get_io_scheduler_count(); ++i) {
            auto* scheduler = m_runtime.get_io_scheduler(i);
            if (scheduler) {
                scheduler->replace_timer_manager(
                    galay::kernel::TimingWheelTimerManager(kLowLatencyIoTimerTickNs));
            }
        }
    }

    bool start_internal() {
        if (m_running.load()) {
            m_start_error = "server is already running";
            return false;
        }
        m_start_error.clear();
        if (!m_stream_handler && !m_active_conn_handler) {
            m_start_error = "missing HTTP/2 stream handler";
            return false;
        }
        if (!init_ssl_context()) {
            return false;
        }

        auto runtime_start = m_runtime.start();
        if (!runtime_start.has_value()) {
            m_start_error = "runtime: " + std::string(runtime_start.error().message());
            HTTP_LOG_ERROR("[h2] [runtime-start-fail]",
                           "error={}",
                           runtime_start.error().message());
            return false;
        }
        m_connections.start(m_runtime);
        configure_low_latency_io_timers();
        if (!start_plugins()) {
            m_start_error = "accept plugin failed to start";
            m_connections.stop(m_runtime);
            m_runtime.stop();
            return false;
        }
        const size_t io_scheduler_count = m_runtime.get_io_scheduler_count();
        if (io_scheduler_count == 0) {
            m_start_error = "server requires at least one IO scheduler";
            stop_started_plugins();
            m_connections.stop(m_runtime);
            m_runtime.stop();
            return false;
        }
        m_listeners.reserve(io_scheduler_count);
        for (size_t i = 0; i < io_scheduler_count; ++i) {
            auto listener = galay::http::server_detail::create_listener(m_config.host, m_config.port, m_config.backlog);
            if (!listener) {
                m_start_error = "listen " + m_config.host + ":" + std::to_string(m_config.port) + ": " + listener.error().message();
                stop_started_plugins();
                m_connections.stop(m_runtime);
                m_runtime.stop();
                m_listeners.clear();
                return false;
            }
            m_listeners.push_back(std::move(*listener));
        }
        m_running.store(true);
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.get_io_scheduler(i);
            if (scheduler) {
                auto loop = server_loop(scheduler, &m_listeners[i]);
                if (!schedule_runtime_task(scheduler, std::move(loop))) {
                    m_start_error = "failed to schedule server loop " + std::to_string(i);
                    m_running.store(false);
                    stop_started_plugins();
                    galay::http::server_detail::close_listeners(m_runtime, m_listeners);
                    m_connections.stop(m_runtime);
                    m_runtime.stop();
                    m_listeners.clear();
                    return false;
                }
            }
        }
        return true;
    }

    bool init_ssl_context() {
        if (!m_ssl_ctx.is_valid()) {
            m_start_error = "TLS context: " + m_ssl_ctx.error().message();
            return false;
        }

        if (m_config.cert_path.empty() || m_config.key_path.empty()) {
            m_start_error = "TLS requires both cert_path and key_path";
            return false;
        }

        auto cert_result = m_ssl_ctx.load_certificate(m_config.cert_path);
        if (!cert_result) {
            m_start_error = "TLS certificate " + m_config.cert_path + ": " + cert_result.error().message();
            return false;
        }

        auto key_result = m_ssl_ctx.load_private_key(m_config.key_path);
        if (!key_result) {
            m_start_error = "TLS private key " + m_config.key_path + ": " + key_result.error().message();
            return false;
        }

        if (!m_config.ca_path.empty()) {
            auto ca_result = m_ssl_ctx.load_ca_certificate(m_config.ca_path);
            if (!ca_result) {
                m_start_error = "TLS CA " + m_config.ca_path + ": " + ca_result.error().message();
                return false;
            }
        }

        if (m_config.verify_peer) {
            m_ssl_ctx.set_verify_mode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.set_verify_depth(m_config.verify_depth);
        } else {
            m_ssl_ctx.set_verify_mode(galay::ssl::SslVerifyMode::None);
        }

        auto alpn_result = m_ssl_ctx.set_alpn_protocols({"h2", "http/1.1"});
        if (!alpn_result) {
            m_start_error = "TLS ALPN: " + alpn_result.error().message();
            return false;
        }
        auto alpn_select_result = m_ssl_ctx.set_alpn_select_protocols({"h2", "http/1.1"});
        if (!alpn_select_result) {
            m_start_error = "TLS ALPN selection: " + alpn_select_result.error().message();
            return false;
        }

        return true;
    }

    Task<void> server_loop(IOScheduler* scheduler, AsyncTcpSocket* listener_socket) {
        if (!listener_socket) co_return;
        auto& listener = *listener_socket;
        // 阶段 1：注册 server_loop 退出守卫，确保循环结束时扣减运行计数
        struct LoopExitGuard {
            H2Server* server;
            ~LoopExitGuard() {
                server->m_listening_loop_count.fetch_sub(1, std::memory_order_acq_rel);
            }
        } guard{this};

        m_listening_loop_count.fetch_add(1, std::memory_order_release);

        // 阶段 8：主 accept 循环，运行期间持续等待新连接
        while (m_running.load()) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);
            // 阶段 9：处理 accept 失败，服务器仍运行时继续循环
            if (!accept_result) {
                if (m_running.load()) {
                }
                continue;
            }

            // 阶段 10：根据 accept 得到的句柄构造 SSL 客户端 socket
            galay::ssl::SslSocket client_socket(&m_ssl_ctx, *accept_result);
            // 阶段 11：配置客户端 socket 的非阻塞与 TCP_NODELAY 选项
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

            // 阶段 12：执行 accept plugin，允许插件在 TLS 握手前拦截连接
            auto continuing_result = co_await run_accept_plugins(client_socket, client_host);
            bool continuing = continuing_result.value_or(false);
            if (!continuing) {
                auto close_result = co_await client_socket.close();
                if (!close_result) {
                    HTTP_LOG_WARN("[socket] [close-fail] [client]",
                                  "error={}",
                                  close_result.error().message());
                }
                continue;
            }

            // 阶段 13：选择连接处理调度器，默认回退到当前 IO 调度器
            auto* target_scheduler = m_runtime.get_next_io_scheduler();
            if (target_scheduler == nullptr) {
                target_scheduler = scheduler;
            }
            // 阶段 14：投递 TLS HTTP/2 连接处理任务，投递失败时关闭客户端 socket
            if (!schedule_runtime_task(target_scheduler, handle_connection(std::move(client_socket)))) {
                auto close_result = co_await client_socket.close();
                if (!close_result) {
                    HTTP_LOG_WARN("[socket] [close-fail] [client]",
                                  "error={}",
                                  close_result.error().message());
                }
            }
        }

        co_return;
    }

    Task<void> read_connection_preface(galay::ssl::SslSocket& socket,
                                     std::array<char, kHttp2ConnectionPrefaceLength>& preface,
                                     bool& ok) {
        ok = false;
        size_t received = 0;
        while (received < preface.size()) {
            auto recv_result = co_await socket.recv(preface.data() + received, preface.size() - received);
            if (!recv_result || recv_result.value().size() == 0) {
                co_return;
            }
            received += recv_result.value().size();
        }
        ok = true;
        co_return;
    }

    Task<void> handle_connection(galay::ssl::SslSocket socket) {
        auto tracked = co_await m_connections.attach(socket.handle().fd);
        if (!tracked) {
            HTTP_LOG_WARN("[h2] [connection-track-fail]", "error={}", tracked.error().message());
            auto close_result = co_await socket.close();
            if (!close_result && close_result.error().code() != kClosed) {
                HTTP_LOG_WARN("[socket] [close-fail]", "context=h2-track-fail error={}", close_result.error().message());
            }
            co_return;
        }
        auto connection_scope = std::move(*tracked);
        auto handshake_result = co_await socket.handshake();
        if (!handshake_result) {
            connection_scope.release_handle();
            auto close_result = co_await socket.close();
            if (!close_result) {
                HTTP_LOG_WARN("[ssl] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            co_return;
        }

        std::string alpn = socket.get_alpn_protocol();
        if (alpn != "h2") {
            co_await handle_http1_fallback(std::move(socket), connection_scope);
            co_return;
        }

        std::array<char, kHttp2ConnectionPrefaceLength> preface{};
        bool preface_ok = false;
        co_await read_connection_preface(socket, preface, preface_ok);
        if (!preface_ok ||
            std::memcmp(preface.data(), kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
            connection_scope.release_handle();
            auto close_result = co_await socket.close();
            if (!close_result) {
                HTTP_LOG_WARN("[ssl] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            co_return;
        }

        Http2ConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        auto local_settings =
            Http2ConnImpl<galay::ssl::SslSocket>::make_settings_frame_from_config(m_config);
        if (conn.apply_local_settings(local_settings) != Http2ErrorCode::NoError) {
            connection_scope.release_handle();
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_WARN("[h2] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            co_return;
        }
        conn.runtime_config().from(m_config);

        auto settings_result = co_await conn.send_settings();
        if (!settings_result) {
            connection_scope.release_handle();
            auto close_result = co_await conn.close();
            if (!close_result) {
                HTTP_LOG_WARN("[h2] [close-fail]",
                              "error={}",
                              close_result.error().message());
            }
            co_return;
        }

        conn.init_stream_manager();
        auto* mgr = conn.stream_manager();
        if (m_active_conn_handler) {
            co_await mgr->start(m_active_conn_handler);
        } else {
            co_await mgr->start(m_stream_handler);
        }
        connection_scope.release_handle();
        auto close_result = co_await conn.close();
        if (!close_result) {
            HTTP_LOG_WARN("[h2] [close-fail]",
                          "error={}",
                          close_result.error().message());
        }
        co_return;
    }

    Task<void> handle_http1_fallback(galay::ssl::SslSocket socket,
                                   galay::http::server_detail::ServerConnections::Scope& connection_scope) {
        galay::http::HttpConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        if (m_http1_fallback) {
            connection_scope.finish();
            co_await m_http1_fallback(std::move(conn));
            co_return;
        }
        co_await run_default_http1_fallback_loop("[h2] [h1-fallback]", std::move(conn), connection_scope);
        co_return;
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

    Task<bool> run_accept_plugins(galay::ssl::SslSocket& client_socket, const Host& client_host) {
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

private:
    Runtime m_runtime;
    H2ServerConfig m_config;
    Http2ConnectionHandler m_stream_handler;
    Http2ActiveConnHandler m_active_conn_handler;
    std::function<Task<void>(galay::http::HttpConnImpl<galay::ssl::SslSocket>)> m_http1_fallback;
    std::vector<std::unique_ptr<galay::http::plugin::AcceptPlugin<galay::ssl::SslSocket>>> m_accept_plugins;
    std::size_t m_started_plugin_count = 0;
    std::atomic<bool> m_running;
    std::atomic<size_t> m_listening_loop_count{0};
    std::vector<AsyncTcpSocket> m_listeners;
    galay::http::server_detail::ServerConnections m_connections;
    std::string m_start_error;
    galay::ssl::SslContext m_ssl_ctx;
    bool m_start_attempted = false;
};

template<bool EnableSwagger>
inline api::ApiResult<std::unique_ptr<H2Server>> H2ServerBuilder<EnableSwagger>::build() {
    return this->template build_native<H2Server>(build_config(), false);
}
#endif

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H
