/**
 * @file http2_server.h
 * @brief HTTP/2 服务器，支持 h2c 升级和 TLS ALPN 协商
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 H2cServer 和 H2Server 模板类，支持两种 HTTP/2 服务模式：
 *          1. h2c 模式：通过 HTTP/1.1 Upgrade 升级到 HTTP/2
 *          2. TLS 模式：通过 ALPN 协议协商直接使用 HTTP/2
 */

#ifndef GALAY_HTTP2_SERVER_H
#define GALAY_HTTP2_SERVER_H

#include "http2/kernel/http2_conn.h"
#include "http2/kernel/stream_mgr.h"
#include "http2/kernel/http2_stream.h"
#include "http/common/iovec_utils.h"
#include "http2/protoc/http2_base.h"
#include "http2/protoc/http2_frame.h"
#include "http/protoc/http_header.h"
#include "http/protoc/http_request.h"
#include "http/common/http_log.h"
#include "http/kernel/http_conn.h"
#include "http/builder/http_builder.h"
#include "http/plugin/common/defn.h"
#include "kernel/async/tcp_socket.h"
#include "kernel/kernel/runtime.h"
#ifdef GALAY_SSL_FEATURE_ENABLED
#include "ssl/ssl/ssl_context.h"
#include "ssl/async/ssl_socket.h"
#endif
#include <memory>
#include <atomic>
#include <exception>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <expected>
#include <string>
#include <array>
#include <optional>
#include <chrono>
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
inline Task<void> runDefaultHttp1FallbackLoop(const char* log_tag,
                                              galay::http::HttpConnImpl<SocketType>&& conn) {
    bool keep_alive = true;
    while (keep_alive) {
        galay::http::HttpRequest request;
        auto reader = conn.getReader();
        auto read_result = co_await reader.getRequest(request);
        if (!read_result) {
            HTTP_LOG_DEBUG("[h1-fallback]",
                           "{} recv failed: {}",
                           log_tag,
                           read_result.error().message());
            break;
        }

        keep_alive = request.header().isKeepAlive() && !request.header().isConnectionClose();

        auto response = galay::http::Http1_1ResponseBuilder()
            .status(galay::http::HttpStatusCode::NotFound_404)
            .header("Content-Type", "text/plain")
            .body("404 Not Found")
            .buildMove();
        auto writer = conn.getWriter();
        auto write_result = co_await writer.sendResponse(response);
        if (!write_result) {
            HTTP_LOG_DEBUG("[h1-fallback]",
                           "{} send failed: {}",
                           log_tag,
                           write_result.error().message());
            break;
        }
    }
    co_await conn.close();
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
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
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
};

class H2cServer;

class H2cServerBuilder {
public:
    H2cServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2cServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2cServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2cServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2cServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    H2cServerBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cServerBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cServerBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cServerBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cServerBuilder& enablePush(bool v)               { m_config.enable_push = v; return *this; }
    H2cServerBuilder& pingEnabled(bool v)              { m_config.ping_enabled = v; return *this; }
    H2cServerBuilder& pingInterval(std::chrono::milliseconds v) { m_config.ping_interval = v; return *this; }
    H2cServerBuilder& pingTimeout(std::chrono::milliseconds v) { m_config.ping_timeout = v; return *this; }
    H2cServerBuilder& settingsAckTimeout(std::chrono::milliseconds v) { m_config.settings_ack_timeout = v; return *this; }
    H2cServerBuilder& gracefulShutdownRtt(std::chrono::milliseconds v) { m_config.graceful_shutdown_rtt = v; return *this; }
    H2cServerBuilder& gracefulShutdownTimeout(std::chrono::milliseconds v) { m_config.graceful_shutdown_timeout = v; return *this; }
    H2cServerBuilder& flowControlTargetWindow(uint32_t v) { m_config.flow_control_target_window = v; return *this; }
    H2cServerBuilder& flowControlStrategy(Http2FlowControlStrategy v) {
        m_config.flow_control_strategy = std::move(v);
        return *this;
    }
    H2cServerBuilder& streamHandler(Http2ConnectionHandler handler) {
        m_config.stream_handler = std::move(handler);
        return *this;
    }
    H2cServerBuilder& activeConnHandler(Http2ActiveConnHandler handler) {
        m_config.active_conn_handler = std::move(handler);
        return *this;
    }
    H2cServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_compute_count = compute_count;
        return *this;
    }
    bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus) {
        if (io_cpus.size() != m_config.io_scheduler_count ||
            compute_cpus.size() != m_config.compute_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(io_cpus);
        m_config.affinity.custom_compute_cpus = std::move(compute_cpus);
        return true;
    }
    H2cServer build() const;
    H2cServerConfig buildConfig() const                { return m_config; }
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
inline bool looksLikeHttpMethod(const char* buf) {
    return buf[0] >= 'A' && buf[0] <= 'Z';
}

/**
 * @brief 从 RingBuffer 的 iovec 拷贝 n 字节到 buf（不 consume）
 */
inline void peekRingBuffer(RingBuffer& rb, char* buf, size_t n) {
    auto iovecs = borrowReadIovecs(rb);
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
inline std::string drainRingBuffer(RingBuffer& rb) {
    std::string data;
    data.reserve(rb.readable());
    auto iovecs = borrowReadIovecs(rb);
    for (const auto& iov : iovecs) {
        data.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
    }
    rb.consume(rb.readable());
    return data;
}

inline void wakeTcpAcceptLoops(const std::string& host, uint16_t port, size_t attempts) {
    if (attempts == 0 || port == 0) {
        return;
    }

    const std::string wake_host =
        (host.empty() || host == "0.0.0.0" || host == "::") ? "127.0.0.1" : host;
    for (size_t i = 0; i < attempts; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, wake_host.c_str(), &addr.sin_addr) == 1) {
            (void)::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        }
        ::close(fd);
    }
}

inline void waitForLoopDrain(const std::atomic<size_t>& loop_count,
                             std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (loop_count.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/**
 * @brief HTTP/1.1 降级处理器类型
 */
using Http1FallbackHandler = std::function<Task<void>(
    galay::http::HttpConnImpl<TcpSocket>, galay::http::HttpRequestHeader)>;

/**
 * @brief h2c 服务器 (HTTP/2 over cleartext)
 */
class H2cServer
{
public:
    explicit H2cServer(const H2cServerConfig& config = H2cServerConfig())
        : m_runtime(RuntimeBuilder().ioSchedulerCount(config.io_scheduler_count)
                                   .computeSchedulerCount(config.compute_scheduler_count)
                                   .applyAffinity(config.affinity)
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
    
    void start() {
        startInternal();
    }

    void start(Http2ConnectionHandler handler) {
        m_stream_handler = std::move(handler);
        m_active_conn_handler = nullptr;
        startInternal();
    }

    void start(Http2ActiveConnHandler handler) {
        m_active_conn_handler = std::move(handler);
        startInternal();
    }

    void setHttp1Fallback(Http1FallbackHandler handler) {
        m_http1_fallback = std::move(handler);
    }
    
    void stop() {
        if (!m_running.load()) {
            stopStartedPlugins();
            return;
        }

        m_running.store(false);
        HTTP_LOG_INFO("[h2c] [server] [stopping]", "port={}", m_config.port);

        wakeTcpAcceptLoops(m_config.host,
                           m_config.port,
                           m_server_loop_count.load(std::memory_order_acquire));
        waitForLoopDrain(m_server_loop_count, std::chrono::milliseconds(100));
        stopStartedPlugins();
        m_runtime.stop();
        HTTP_LOG_INFO("[h2c] [server] [stopped]", "port={}", m_config.port);
    }
    
    bool isRunning() const {
        return m_running.load();
    }
    
    Runtime& getRuntime() {
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
    bool addAcceptPlugin(std::unique_ptr<galay::http::plugin::AcceptPlugin<TcpSocket>> plugin) {
        if (m_running.load() || !plugin) {
            return false;
        }

        m_accept_plugins.push_back(std::move(plugin));
        return true;
    }

private:
    bool startInternal() {
        if (m_running.load()) {
            HTTP_LOG_WARN("[h2c] [server]", "already running");
            return false;
        }

        if (!m_stream_handler && !m_active_conn_handler) {
            HTTP_LOG_ERROR("[h2c] [handler]", "missing");
            return false;
        }

        m_runtime.start();

        if (!startPlugins()) {
            m_runtime.stop();
            return false;
        }

        m_running.store(true);
        HTTP_LOG_INFO("[server] [listen] [h2c]",
                      "host={} port={}",
                      m_config.host,
                      m_config.port);

        // Spawn one serverLoop per IO scheduler with SO_REUSEPORT
        size_t io_scheduler_count = m_runtime.getIOSchedulerCount();
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                auto loop = serverLoop(scheduler);
                m_server_loop_count.fetch_add(1, std::memory_order_acq_rel);
                if (!scheduleTask(scheduler, std::move(loop))) {
                    m_server_loop_count.fetch_sub(1, std::memory_order_acq_rel);
                    HTTP_LOG_ERROR("[h2c] [schedule-fail]", "server-loop");
                    m_running.store(false);
                    wakeTcpAcceptLoops(m_config.host,
                                       m_config.port,
                                       m_server_loop_count.load(std::memory_order_acquire));
                    waitForLoopDrain(m_server_loop_count, std::chrono::milliseconds(100));
                    stopStartedPlugins();
                    m_runtime.stop();
                    return false;
                }
            }
        }

        return true;
    }

    Task<void> serverLoop(IOScheduler* scheduler) {
        // 阶段 1：注册 serverLoop 退出守卫，确保循环结束时扣减运行计数
        struct LoopExitGuard {
            H2cServer* server;
            ~LoopExitGuard() {
                server->m_server_loop_count.fetch_sub(1, std::memory_order_acq_rel);
            }
        } guard{this};

        // 阶段 2：创建当前 IO 调度器专属的 listener socket
        // Each serverLoop creates its own listener socket
        std::optional<TcpSocket> listener_opt;
        try {
            listener_opt.emplace(IPType::IPV4);
        } catch (...) {
            HTTP_LOG_ERROR("[socket] [create-fail] [listener]", "tcp listener construction failed");
            co_return;
        }
        TcpSocket& listener = *listener_opt;

        // 阶段 3：配置 listener 复用地址，允许快速重启绑定同一地址
        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            HTTP_LOG_ERROR("[socket] [reuseaddr-fail]",
                           "error={}",
                           reuse_result.error().message());
            co_return;
        }

        // 阶段 4：配置 listener 复用端口，支持多 IO 调度器并行 accept
        auto reuse_port_result = listener.option().handleReusePort();
        if (!reuse_port_result) {
            HTTP_LOG_ERROR("[socket] [reuseport-fail]",
                           "error={}",
                           reuse_port_result.error().message());
            co_return;
        }

        // 阶段 5：设置 listener 为非阻塞模式，交给协程调度器驱动 IO
        auto nonblock_result = listener.option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("[socket] [nonblock-fail]",
                           "error={}",
                           nonblock_result.error().message());
            co_return;
        }

        // 阶段 6：绑定监听地址和端口
        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(bind_host);
        if (!bind_result) {
            HTTP_LOG_ERROR("[bind] [fail]",
                           "host={} port={} error={}",
                           m_config.host,
                           m_config.port,
                           bind_result.error().message());
            co_return;
        }

        // 阶段 7：进入 listen 状态，准备接收 h2c 连接
        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            HTTP_LOG_ERROR("[listen] [fail]",
                           "error={}",
                           listen_result.error().message());
            co_return;
        }

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
            std::optional<TcpSocket> client_socket_opt;
            try {
                client_socket_opt.emplace(*accept_result);
            } catch (...) {
                HTTP_LOG_ERROR("[socket] [create-fail] [client]", "tcp client construction failed");
                continue;
            }
            TcpSocket client_socket = std::move(*client_socket_opt);
            // 阶段 11：配置客户端 socket 为非阻塞模式
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                HTTP_LOG_ERROR("[socket] [nonblock-fail] [client]",
                               "error={}",
                               nonblock_result.error().message());
                continue;
            }

            // 阶段 12：执行 accept plugin，允许插件在协议检测前拦截连接
            auto continuing_result = co_await runAcceptPlugins(client_socket, client_host);
            bool continuing = continuing_result.value_or(false);
            if (!continuing) {
                co_await client_socket.close();
                continue;
            }

            // 阶段 13：把 h2c 连接处理任务投递到当前 IO 调度器
            // Handle connection on the same scheduler
            if (!scheduleTask(scheduler, handleConnection(std::move(client_socket)))) {
                HTTP_LOG_ERROR("[h2c] [schedule-fail]", "handle-connection");
                co_await client_socket.close();
            }
        }

        // 阶段 13：serverLoop 退出前关闭 listener socket
        auto close_result = co_await listener.close();
        if (!close_result) {
            HTTP_LOG_WARN("[socket] [close-fail] [listener]",
                          "error={}",
                          close_result.error().message());
        }
        co_return;
    }
    
    /**
     * @brief 处理新连接
     */
    Task<void> handleConnection(TcpSocket socket) {
        Http2ConnImpl<TcpSocket> conn(std::move(socket));

        // 配置本地设置
        conn.localSettings().from(m_config);
        conn.runtimeConfig().from(m_config);

        DetectedProtocol protocol = DetectedProtocol::Unknown;
        galay::http::HttpRequestHeader upgrade_request;
        co_await detectProtocol(conn, protocol, upgrade_request);

        switch (protocol) {
        case DetectedProtocol::H2cPriorKnowledge:
        case DetectedProtocol::H2cUpgrade: {
            // 初始化 StreamManager 并启动帧分发循环
            conn.initStreamManager();
            auto* mgr = conn.streamManager();
            HTTP_LOG_DEBUG("[h2] [stream-mgr]", "starting");
            if (m_active_conn_handler) {
                co_await mgr->start(m_active_conn_handler);
            } else {
                co_await mgr->start(m_stream_handler);
            }
            HTTP_LOG_DEBUG("[h2] [stream-mgr]", "stopped");
            co_await conn.close();
            break;
        }
        case DetectedProtocol::Http1:
            co_await handleHttp1Fallback(std::move(conn), std::move(upgrade_request));
            break;
        default:
            HTTP_LOG_ERROR("[protocol] [detect-fail]", "h2c unknown");
            co_await conn.close();
            break;
        }

        co_return;
    }

    Task<void> readAtLeast(Http2ConnImpl<TcpSocket>& conn, size_t n) {
        auto& rb = conn.ringBuffer();
        while (rb.readable() < n) {
            auto write_iovecs = borrowWriteIovecs(rb);
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
    Task<void> detectProtocol(Http2ConnImpl<TcpSocket>& conn,
                              DetectedProtocol& protocol,
                              galay::http::HttpRequestHeader& upgrade_request) {
        protocol = DetectedProtocol::Unknown;
        auto& rb = conn.ringBuffer();

        co_await readAtLeast(conn, kHttp2ConnectionPrefaceLength);
        if (rb.readable() < kHttp2ConnectionPrefaceLength) {
            co_return;
        }

        char peek_buf[kHttp2ConnectionPrefaceLength];
        peekRingBuffer(rb, peek_buf, kHttp2ConnectionPrefaceLength);

        // ===== Prior Knowledge =====
        if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) == 0) {
            HTTP_LOG_DEBUG("[h2] [prior-knowledge]", "detected");
            rb.consume(kHttp2ConnectionPrefaceLength);

            auto settings_result = co_await conn.sendSettings();
            if (!settings_result) {
                co_return;
            }

            protocol = DetectedProtocol::H2cPriorKnowledge;
            co_return;
        }

        // ===== HTTP/1.1 (Upgrade or fallback) =====
        if (looksLikeHttpMethod(peek_buf)) {
            HTTP_LOG_DEBUG("[h1] [detect]", "h2c fallback or upgrade");

            std::string header_data = drainRingBuffer(rb);
            while (header_data.find("\r\n\r\n") == std::string::npos && header_data.size() < 8192) {
                auto write_iovecs = borrowWriteIovecs(rb);
                auto result = co_await conn.socket().readv(write_iovecs.storage(), write_iovecs.size());
                if (!result || result.value() == 0) {
                    co_return;
                }
                rb.produce(result.value());
                header_data.append(drainRingBuffer(rb));
            }

            size_t header_end = header_data.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                HTTP_LOG_ERROR("[header] [invalid]", "too large");
                co_return;
            }

            auto parse_result = upgrade_request.fromString(
                std::string_view(header_data.data(), header_end + 4));
            if (parse_result.first != galay::http::kNoError || parse_result.second <= 0) {
                HTTP_LOG_ERROR("[header] [parse-fail]",
                               "code={}",
                               static_cast<int>(parse_result.first));
                co_return;
            }

            auto& headers = upgrade_request.headerPairs();
            std::string upgrade_value = headers.getValue("Upgrade");
            std::transform(upgrade_value.begin(), upgrade_value.end(), upgrade_value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            bool has_upgrade = (upgrade_value == "h2c");
            bool has_http2_settings = headers.hasKey("HTTP2-Settings");

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
                    rb.write(header_data.data() + header_end + 4,
                             header_data.size() - header_end - 4);
                }

                co_await readAtLeast(conn, kHttp2ConnectionPrefaceLength);
                if (rb.readable() < kHttp2ConnectionPrefaceLength) {
                    HTTP_LOG_ERROR("[preface] [recv-fail]", "h2c");
                    co_return;
                }

                peekRingBuffer(rb, peek_buf, kHttp2ConnectionPrefaceLength);
                if (std::memcmp(peek_buf, kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
                    HTTP_LOG_ERROR("[preface] [invalid]", "after upgrade");
                    co_return;
                }
                HTTP_LOG_DEBUG("[preface] [ok]", "h2c");

                rb.consume(kHttp2ConnectionPrefaceLength);

                auto settings_result = co_await conn.sendSettings();
                if (!settings_result) {
                    co_return;
                }

                protocol = DetectedProtocol::H2cUpgrade;
                co_return;
            }

            // 回退到 HTTP/1.1 链路时，需要把已经读出的首个请求头（和可能携带的 body）
            // 回灌到 RingBuffer，交给标准 HttpReader 继续解析。
            if (!header_data.empty() && !m_http1_fallback) {
                rb.write(header_data.data(), header_data.size());
            }
            protocol = DetectedProtocol::Http1;
            co_return;
        }

        HTTP_LOG_WARN("[protocol] [unknown]", "h2c");
        co_return;
    }

    Task<void> handleHttp1Fallback(Http2ConnImpl<TcpSocket>&& h2_conn,
                                   galay::http::HttpRequestHeader first_request_header) {
        galay::http::HttpConnImpl<TcpSocket> conn(
            std::move(h2_conn.socket()), std::move(h2_conn.ringBuffer()));

        if (m_http1_fallback) {
            co_await m_http1_fallback(std::move(conn), std::move(first_request_header));
            co_return;
        }

        (void)first_request_header;
        // 默认行为：进入 HTTP/1.1 处理链路，而不是直接返回 505。
        co_await runDefaultHttp1FallbackLoop("[h2c] [h1-fallback]", std::move(conn));
        co_return;
    }

    bool startPlugins() {
        m_started_plugin_count = 0;
        for (auto& plugin : m_accept_plugins) {
            try {
                if (!plugin->start(m_runtime)) {
                    stopStartedPlugins();
                    return false;
                }
                ++m_started_plugin_count;
            } catch (const std::exception& ex) {
                HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error={}", ex.what());
                stopStartedPlugins();
                return false;
            } catch (...) {
                HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error=unknown");
                stopStartedPlugins();
                return false;
            }
        }
        return true;
    }

    void stopStartedPlugins() noexcept {
        while (m_started_plugin_count > 0) {
            --m_started_plugin_count;
            try {
                m_accept_plugins[m_started_plugin_count]->stop();
            } catch (...) {
                HTTP_LOG_ERROR("[accept-plugin] [stop-fail]", "error=exception");
            }
        }
    }

    Task<bool> runAcceptPlugins(TcpSocket& client_socket, const Host& client_host) {
        for (auto& plugin : m_accept_plugins) {
            auto plugin_result = co_await plugin->handle(getRuntime(), client_socket, client_host);
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
    std::vector<std::unique_ptr<galay::http::plugin::AcceptPlugin<TcpSocket>>> m_accept_plugins;
    std::size_t m_started_plugin_count = 0;
    std::atomic<bool> m_running;
    std::atomic<size_t> m_server_loop_count{0};
};

inline H2cServer H2cServerBuilder::build() const { return H2cServer(m_config); }

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief h2 服务器配置（HTTP/2 over TLS）
 */
struct H2ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 9443;
    int backlog = 128;
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;
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
};

class H2Server;

class H2ServerBuilder {
public:
    H2ServerBuilder& host(std::string v)              { m_config.host = std::move(v); return *this; }
    H2ServerBuilder& port(uint16_t v)                 { m_config.port = v; return *this; }
    H2ServerBuilder& backlog(int v)                   { m_config.backlog = v; return *this; }
    H2ServerBuilder& ioSchedulerCount(size_t v)       { m_config.io_scheduler_count = v; return *this; }
    H2ServerBuilder& computeSchedulerCount(size_t v)  { m_config.compute_scheduler_count = v; return *this; }
    H2ServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_compute_count = compute_count;
        return *this;
    }
    bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus) {
        if (io_cpus.size() != m_config.io_scheduler_count ||
            compute_cpus.size() != m_config.compute_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(io_cpus);
        m_config.affinity.custom_compute_cpus = std::move(compute_cpus);
        return true;
    }
    H2ServerBuilder& certPath(std::string v)          { m_config.cert_path = std::move(v); return *this; }
    H2ServerBuilder& keyPath(std::string v)           { m_config.key_path = std::move(v); return *this; }
    H2ServerBuilder& caPath(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    H2ServerBuilder& verifyPeer(bool v)               { m_config.verify_peer = v; return *this; }
    H2ServerBuilder& verifyDepth(int v)               { m_config.verify_depth = v; return *this; }
    H2ServerBuilder& maxConcurrentStreams(uint32_t v) { m_config.max_concurrent_streams = v; return *this; }
    H2ServerBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2ServerBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2ServerBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2ServerBuilder& enablePush(bool v)               { m_config.enable_push = v; return *this; }
    H2ServerBuilder& pingEnabled(bool v)              { m_config.ping_enabled = v; return *this; }
    H2ServerBuilder& pingInterval(std::chrono::milliseconds v) { m_config.ping_interval = v; return *this; }
    H2ServerBuilder& pingTimeout(std::chrono::milliseconds v) { m_config.ping_timeout = v; return *this; }
    H2ServerBuilder& settingsAckTimeout(std::chrono::milliseconds v) { m_config.settings_ack_timeout = v; return *this; }
    H2ServerBuilder& gracefulShutdownRtt(std::chrono::milliseconds v) { m_config.graceful_shutdown_rtt = v; return *this; }
    H2ServerBuilder& gracefulShutdownTimeout(std::chrono::milliseconds v) { m_config.graceful_shutdown_timeout = v; return *this; }
    H2ServerBuilder& flowControlTargetWindow(uint32_t v) { m_config.flow_control_target_window = v; return *this; }
    H2ServerBuilder& flowControlStrategy(Http2FlowControlStrategy v) {
        m_config.flow_control_strategy = std::move(v);
        return *this;
    }
    H2ServerBuilder& streamHandler(Http2ConnectionHandler handler) {
        m_config.stream_handler = std::move(handler);
        return *this;
    }
    H2ServerBuilder& activeConnHandler(Http2ActiveConnHandler handler) {
        m_config.active_conn_handler = std::move(handler);
        return *this;
    }
    H2Server build() const;
    H2ServerConfig buildConfig() const                { return m_config; }
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
        : m_runtime(RuntimeBuilder().ioSchedulerCount(config.io_scheduler_count)
                                   .computeSchedulerCount(config.compute_scheduler_count)
                                   .applyAffinity(config.affinity)
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

    void start() {
        startInternal();
    }

    void start(Http2ConnectionHandler handler) {
        m_stream_handler = std::move(handler);
        m_active_conn_handler = nullptr;
        startInternal();
    }

    void start(Http2ActiveConnHandler handler) {
        m_active_conn_handler = std::move(handler);
        startInternal();
    }

    void setHttp1Fallback(
        std::function<Task<void>(galay::http::HttpConnImpl<galay::ssl::SslSocket>)> handler) {
        m_http1_fallback = std::move(handler);
    }

    void stop() {
        if (!m_running.load()) {
            stopStartedPlugins();
            return;
        }

        m_running.store(false);
        wakeTcpAcceptLoops(m_config.host,
                           m_config.port,
                           m_server_loop_count.load(std::memory_order_acquire));
        waitForLoopDrain(m_server_loop_count, std::chrono::milliseconds(100));
        stopStartedPlugins();
        m_runtime.stop();
    }

    bool isRunning() const {
        return m_running.load();
    }

    Runtime& getRuntime() {
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
    bool addAcceptPlugin(std::unique_ptr<galay::http::plugin::AcceptPlugin<galay::ssl::SslSocket>> plugin) {
        if (m_running.load() || !plugin) {
            return false;
        }

        m_accept_plugins.push_back(std::move(plugin));
        return true;
    }

private:
    static constexpr uint64_t kLowLatencyIoTimerTickNs = 1000000ULL;

    void configureLowLatencyIoTimers() {
        for (size_t i = 0; i < m_runtime.getIOSchedulerCount(); ++i) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                scheduler->replaceTimerManager(
                    galay::kernel::TimingWheelTimerManager(kLowLatencyIoTimerTickNs));
            }
        }
    }

    bool startInternal() {
        if (m_running.load()) {
            return false;
        }
        if (!m_stream_handler && !m_active_conn_handler) {
            return false;
        }
        if (!initSslContext()) {
            return false;
        }

        m_runtime.start();
        configureLowLatencyIoTimers();
        if (!startPlugins()) {
            m_runtime.stop();
            return false;
        }
        m_running.store(true);

        size_t io_scheduler_count = m_runtime.getIOSchedulerCount();
        for (size_t i = 0; i < io_scheduler_count; i++) {
            auto* scheduler = m_runtime.getIOScheduler(i);
            if (scheduler) {
                auto loop = serverLoop(scheduler);
                m_server_loop_count.fetch_add(1, std::memory_order_acq_rel);
                if (!scheduleTask(scheduler, std::move(loop))) {
                    m_server_loop_count.fetch_sub(1, std::memory_order_acq_rel);
                    m_running.store(false);
                    wakeTcpAcceptLoops(m_config.host,
                                       m_config.port,
                                       m_server_loop_count.load(std::memory_order_acquire));
                    waitForLoopDrain(m_server_loop_count, std::chrono::milliseconds(100));
                    stopStartedPlugins();
                    m_runtime.stop();
                    return false;
                }
            }
        }
        return true;
    }

    bool initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            return false;
        }

        if (m_config.cert_path.empty() || m_config.key_path.empty()) {
            return false;
        }

        auto cert_result = m_ssl_ctx.loadCertificate(m_config.cert_path);
        if (!cert_result) {
            return false;
        }

        auto key_result = m_ssl_ctx.loadPrivateKey(m_config.key_path);
        if (!key_result) {
            return false;
        }

        if (!m_config.ca_path.empty()) {
            auto ca_result = m_ssl_ctx.loadCACertificate(m_config.ca_path);
            if (!ca_result) {
                return false;
            }
        }

        if (m_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_config.verify_depth);
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }

        auto alpn_result = m_ssl_ctx.setALPNProtocols({"h2", "http/1.1"});
        if (!alpn_result) {
            return false;
        }
        auto alpn_select_result = m_ssl_ctx.setALPNSelectProtocols({"h2", "http/1.1"});
        if (!alpn_select_result) {
            return false;
        }

        return true;
    }

    Task<void> serverLoop(IOScheduler* scheduler) {
        // 阶段 1：注册 serverLoop 退出守卫，确保循环结束时扣减运行计数
        struct LoopExitGuard {
            H2Server* server;
            ~LoopExitGuard() {
                server->m_server_loop_count.fetch_sub(1, std::memory_order_acq_rel);
            }
        } guard{this};

        // 阶段 2：创建当前 IO 调度器专属的 TCP listener socket
        std::optional<TcpSocket> listener_opt;
        try {
            listener_opt.emplace(IPType::IPV4);
        } catch (...) {
            co_return;
        }
        TcpSocket& listener = *listener_opt;

        // 阶段 3：配置 listener 复用地址，允许快速重启绑定同一地址
        auto reuse_result = listener.option().handleReuseAddr();
        if (!reuse_result) {
            co_return;
        }

        // 阶段 4：配置 listener 复用端口，支持多 IO 调度器并行 accept
        auto reuse_port_result = listener.option().handleReusePort();
        if (!reuse_port_result) {
            co_return;
        }

        // 阶段 5：设置 listener 为非阻塞模式，交给协程调度器驱动 IO
        auto nonblock_result = listener.option().handleNonBlock();
        if (!nonblock_result) {
            co_return;
        }

        // 阶段 6：绑定监听地址和端口
        Host bind_host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(bind_host);
        if (!bind_result) {
            co_return;
        }

        // 阶段 7：进入 listen 状态，准备接收 TLS HTTP/2 连接
        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            co_return;
        }

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
            std::optional<galay::ssl::SslSocket> client_socket_opt;
            try {
                client_socket_opt.emplace(&m_ssl_ctx, *accept_result);
            } catch (...) {
                continue;
            }
            galay::ssl::SslSocket client_socket = std::move(*client_socket_opt);
            // 阶段 11：配置客户端 socket 的非阻塞与 TCP_NODELAY 选项
            auto nonblock_result = client_socket.option().handleNonBlock();
            if (!nonblock_result) {
                continue;
            }
            auto nodelay_result = client_socket.option().handleTcpNoDelay();
            if (!nodelay_result) {
            }

            // 阶段 12：执行 accept plugin，允许插件在 TLS 握手前拦截连接
            auto continuing_result = co_await runAcceptPlugins(client_socket, client_host);
            bool continuing = continuing_result.value_or(false);
            if (!continuing) {
                co_await client_socket.close();
                continue;
            }

            // 阶段 13：选择连接处理调度器，默认回退到当前 IO 调度器
            auto* target_scheduler = m_runtime.getNextIOScheduler();
            if (target_scheduler == nullptr) {
                target_scheduler = scheduler;
            }
            // 阶段 14：投递 TLS HTTP/2 连接处理任务，投递失败时关闭客户端 socket
            if (!scheduleTask(target_scheduler, handleConnection(std::move(client_socket)))) {
                co_await client_socket.close();
            }
        }

        // 阶段 14：serverLoop 退出前关闭 listener socket
        auto close_result = co_await listener.close();
        if (!close_result) {
        }
        co_return;
    }

    Task<void> readConnectionPreface(galay::ssl::SslSocket& socket,
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

    Task<void> handleConnection(galay::ssl::SslSocket socket) {
        auto handshake_result = co_await socket.handshake();
        if (!handshake_result) {
            co_await socket.close();
            co_return;
        }

        std::string alpn = socket.getALPNProtocol();
        if (alpn != "h2") {
            co_await handleHttp1Fallback(std::move(socket));
            co_return;
        }

        std::array<char, kHttp2ConnectionPrefaceLength> preface{};
        bool preface_ok = false;
        co_await readConnectionPreface(socket, preface, preface_ok);
        if (!preface_ok ||
            std::memcmp(preface.data(), kHttp2ConnectionPreface.data(), kHttp2ConnectionPrefaceLength) != 0) {
            co_await socket.close();
            co_return;
        }

        Http2ConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        conn.localSettings().from(m_config);
        conn.runtimeConfig().from(m_config);

        auto settings_result = co_await conn.sendSettings();
        if (!settings_result) {
            co_await conn.close();
            co_return;
        }

        conn.initStreamManager();
        auto* mgr = conn.streamManager();
        if (m_active_conn_handler) {
            co_await mgr->start(m_active_conn_handler);
        } else {
            co_await mgr->start(m_stream_handler);
        }
        co_await conn.close();
        co_return;
    }

    Task<void> handleHttp1Fallback(galay::ssl::SslSocket socket) {
        galay::http::HttpConnImpl<galay::ssl::SslSocket> conn(std::move(socket));
        if (m_http1_fallback) {
            co_await m_http1_fallback(std::move(conn));
            co_return;
        }
        co_await runDefaultHttp1FallbackLoop("[h2] [h1-fallback]", std::move(conn));
        co_return;
    }

    bool startPlugins() {
        m_started_plugin_count = 0;
        for (auto& plugin : m_accept_plugins) {
            try {
                if (!plugin->start(m_runtime)) {
                    stopStartedPlugins();
                    return false;
                }
                ++m_started_plugin_count;
            } catch (const std::exception& ex) {
                HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error={}", ex.what());
                stopStartedPlugins();
                return false;
            } catch (...) {
                HTTP_LOG_ERROR("[accept-plugin] [start-fail]", "error=unknown");
                stopStartedPlugins();
                return false;
            }
        }
        return true;
    }

    void stopStartedPlugins() noexcept {
        while (m_started_plugin_count > 0) {
            --m_started_plugin_count;
            try {
                m_accept_plugins[m_started_plugin_count]->stop();
            } catch (...) {
                HTTP_LOG_ERROR("[accept-plugin] [stop-fail]", "error=exception");
            }
        }
    }

    Task<bool> runAcceptPlugins(galay::ssl::SslSocket& client_socket, const Host& client_host) {
        for (auto& plugin : m_accept_plugins) {
            auto plugin_result = co_await plugin->handle(getRuntime(), client_socket, client_host);
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
    std::atomic<size_t> m_server_loop_count{0};
    galay::ssl::SslContext m_ssl_ctx;
};

inline H2Server H2ServerBuilder::build() const { return H2Server(m_config); }
#endif

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H
