/**
 * @file rpc_server.h
 * @brief RPC服务器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC服务器功能，支持服务注册和请求分发。
 *
 * @example
 * @code
 * // 创建服务
 * EchoService echoService;
 *
 * // 启动服务器
 * auto server = RpcServerBuilder()
 *     .host("0.0.0.0")
 *     .port(9000)
 *     .build();
 * auto registered = server.register_service(echoService);
 * if (!registered) {
 *     return;
 * }
 * auto started = server.start();
 * @endcode
 */

#ifndef GALAY_RPC_SERVER_H
#define GALAY_RPC_SERVER_H

#include "../common/rpc_log.h"
#include "rpc_service.h"
#include "rpc_conn.h"
#include "rpc_interceptor.h"
#include "../utils/runtime_compat.h"
#include "../../galay-kernel/core/runtime.h"
#include "../../galay-kernel/async/async_tcp.h"
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <atomic>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief RPC服务器配置
 */
struct RpcServerConfig {
    std::string host = "0.0.0.0";       ///< 监听地址
    uint16_t port = 9000;               ///< 监听端口
    int backlog = 128;                  ///< 监听队列长度
    bool tcp_no_delay = true;           ///< 是否为已接受连接启用 TCP_NODELAY
    size_t io_scheduler_count = 0;      ///< IO调度器数量，0表示自动
    size_t parallel_scheduler_count = 0; ///< 计算调度器数量，0表示自动
    RuntimeAffinityConfig affinity;     ///< 绑核配置
    size_t ring_buffer_size = kDefaultRpcRingBufferSize;  ///< RingBuffer大小
    RpcServerInterceptor interceptor = allow_all_rpc_interceptor();  ///< 请求前置拦截器
};

class RpcServer;

/**
 * @brief RPC服务器构建器
 *
 * @details 使用Builder模式配置并创建RpcServer实例。
 */
class RpcServerBuilder {
public:
    /// @brief 设置监听地址
    RpcServerBuilder& host(std::string value)                            { m_config.host = std::move(value); return *this; }
    /// @brief 设置监听端口
    RpcServerBuilder& port(uint16_t value)                               { m_config.port = value; return *this; }
    /// @brief 设置监听队列长度
    RpcServerBuilder& backlog(int value)                                 { m_config.backlog = value; return *this; }
    /// @brief 设置已接受连接是否启用 TCP_NODELAY
    RpcServerBuilder& tcp_no_delay(bool value)                             { m_config.tcp_no_delay = value; return *this; }
    /// @brief 设置IO调度器数量
    RpcServerBuilder& io_scheduler_count(size_t value)                     { m_config.io_scheduler_count = value; return *this; }
    /// @brief 设置计算调度器数量
    RpcServerBuilder& parallel_scheduler_count(size_t value)                { m_config.parallel_scheduler_count = value; return *this; }
    /**
     * @brief 设置顺序绑核策略
     * @param io_count IO调度器绑核数
     * @param parallel_count 计算调度器绑核数
     * @return 构建器引用
     */
    RpcServerBuilder& sequential_affinity(size_t io_count, size_t parallel_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_parallel_count = parallel_count;
        return *this;
    }
    /**
     * @brief 设置自定义绑核策略
     * @param io_cpus IO调度器绑定的CPU列表
     * @param parallel_cpus 计算调度器绑定的CPU列表
     * @return 是否设置成功（数量必须匹配调度器数量）
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
    /// @brief 设置环形缓冲区大小
    RpcServerBuilder& ring_buffer_size(size_t value)                       { m_config.ring_buffer_size = value; return *this; }
    /// @brief 设置请求前置拦截器
    RpcServerBuilder& interceptor(RpcServerInterceptor value)             { m_config.interceptor = std::move(value); return *this; }
    /// @brief 构建RpcServer实例
    RpcServer build() const;
    /// @brief 仅导出配置
    RpcServerConfig build_config() const                                  { return m_config; }

private:
    RpcServerConfig m_config;  ///< 服务器配置
};

/**
 * @brief RPC服务器
 *
 * @details 提供一元RPC服务器功能，支持服务注册、路由缓存和请求分发。
 *          每个连接由协程驱动，使用RingBuffer配合readv/writev进行高效IO。
 */
class RpcServer {
public:
    static constexpr size_t kMaxRegisteredServices = 64;  ///< 单个服务器最多注册的服务数

    /**
     * @brief 构造函数
     * @param config 服务器配置
     */
    explicit RpcServer(const RpcServerConfig& config)
        : m_config(config)
        , m_runtime(RuntimeBuilder()
                        .io_scheduler_count(resolve_io_scheduler_count(config.io_scheduler_count))
                        .parallel_scheduler_count(config.parallel_scheduler_count)
                        .apply_affinity(config.affinity)
                        .build()) {}

    ~RpcServer() {
        stop();
    }

    /**
     * @brief 注册服务
     * @param service 服务实例；服务器不取得所有权，实例必须存活到服务器停止之后
     * @return 成功返回void；服务名为空或重复时返回INVALID_REQUEST，容量耗尽时返回RESOURCE_EXHAUSTED
     * @note 注册表使用固定内联存储，调用过程不执行堆分配；仅可在start()之前调用
     */
    std::expected<void, RpcError> register_service(RpcService& service) {
        if (m_running.load(std::memory_order_acquire)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "Cannot register RPC service after server start"));
        }
        if (service.name().empty()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC service name is empty"));
        }
        const size_t initial_index = service_bucket_index(service.name());
        for (size_t probe = 0; probe < m_services.size(); ++probe) {
            RpcService*& slot = m_services[(initial_index + probe) % m_services.size()];
            if (slot == nullptr) {
                slot = &service;
                return {};
            }
            if (slot->name() == service.name()) {
                return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                                "Duplicate RPC service name"));
            }
        }
        return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                        "RPC service registry capacity exceeded"));
    }

    /**
     * @brief 启动服务器
     * @return 成功返回void；runtime、socket、bind、listen或任务调度失败时返回RpcError
     * @note 返回成功时监听socket已经完成bind/listen；该函数不阻塞等待accept循环结束
     */
    std::expected<void, RpcError> start() {
        if (m_running.load(std::memory_order_acquire)) {
            return {};
        }
        RPC_LOG_INFO("[server] [start]",
                     "host={} port={} backlog={}",
                     m_config.host,
                     m_config.port,
                     m_config.backlog);
        m_last_error.reset();

        auto runtime_started = m_runtime.start();
        if (!runtime_started.has_value()) {
            RpcError error(RpcErrorCode::INTERNAL_ERROR,
                           runtime_started.error().message());
            RPC_LOG_ERROR("[server] [runtime] [start-fail]",
                          "error={}", error.message());
            return std::unexpected(std::move(error));
        }

        auto listener_result = AsyncTcpSocket::create(IPType::IPV4);
        if (!listener_result.has_value()) {
            RpcError error = RpcError::from(listener_result.error());
            RPC_LOG_ERROR("[server] [socket] [create-fail]",
                          "error={}", error.message());
            m_runtime.stop();
            return std::unexpected(std::move(error));
        }
        AsyncTcpSocket listener = std::move(*listener_result);

        auto reuse_addr_result = listener.option().handle_reuse_addr();
        if (!reuse_addr_result.has_value()) {
            RpcError error = RpcError::from(reuse_addr_result.error());
            RPC_LOG_ERROR("[server] [socket] [reuseaddr-fail]",
                          "error={}", error.message());
            m_runtime.stop();
            return std::unexpected(std::move(error));
        }
        auto non_block_result = listener.option().handle_non_block();
        if (!non_block_result.has_value()) {
            RpcError error = RpcError::from(non_block_result.error());
            RPC_LOG_ERROR("[server] [socket] [nonblock-fail]",
                          "error={}", error.message());
            m_runtime.stop();
            return std::unexpected(std::move(error));
        }

        Host host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(host);
        if (!bind_result.has_value()) {
            RpcError error = RpcError::from(bind_result.error());
            RPC_LOG_ERROR("[server] [bind] [fail]",
                          "host={} port={} error={}",
                          m_config.host,
                          m_config.port,
                          error.message());
            m_runtime.stop();
            return std::unexpected(std::move(error));
        }
        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result.has_value()) {
            RpcError error = RpcError::from(listen_result.error());
            RPC_LOG_ERROR("[server] [listen] [fail]",
                          "port={} backlog={} error={}",
                          m_config.port,
                          m_config.backlog,
                          error.message());
            m_runtime.stop();
            return std::unexpected(std::move(error));
        }

        m_running.store(true, std::memory_order_release);
        auto* scheduler = m_runtime.get_next_io_scheduler();
        if (!schedule_task(scheduler, accept_loop(std::move(listener)))) {
            RpcError error(RpcErrorCode::INTERNAL_ERROR,
                           "Failed to schedule accept loop");
            RPC_LOG_ERROR("[server] [schedule] [fail]", "accept-loop");
            m_running.store(false, std::memory_order_release);
            m_runtime.stop();
            return std::unexpected(std::move(error));
        }
        return {};
    }

    /**
     * @brief 停止服务器
     */
    void stop() {
        if (m_running.exchange(false, std::memory_order_acq_rel)) {
            RPC_LOG_INFO("[server] [stop]", "port={}", m_config.port);
            m_runtime.stop();
        }
    }

    /**
     * @brief 检查是否运行中
     */
    bool is_running() const {
        return m_running.load(std::memory_order_acquire);
    }

    /**
     * @brief 获取Runtime
     */
    Runtime& runtime() { return m_runtime; }

    /**
     * @brief 获取最近一次异步运行错误（若有）
     * @note 非线程安全，仅保留兼容诊断用途；启动失败必须读取start()返回值。
     */
    std::optional<RpcError> last_error() const {
        return m_last_error;
    }

private:
    static constexpr size_t kRouteCacheSize = 8;
    static constexpr size_t kRouteStringReserve = 32;
    static constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    static constexpr uint64_t kFnvPrime = 1099511628211ull;

    struct RouteCacheEntry {
        uint64_t route_hash = 0;
        std::string service_name;
        std::string method_name;
        RpcCallMode call_mode = RpcCallMode::UNARY;
        RpcMethodHandler* handler = nullptr;
    };

    static uint64_t hash_append(uint64_t hash, std::string_view str) {
        for (unsigned char ch : str) {
            hash ^= static_cast<uint64_t>(ch);
            hash *= kFnvPrime;
        }
        return hash;
    }

    static size_t service_bucket_index(std::string_view service_name) {
        return static_cast<size_t>(hash_append(kFnvOffset, service_name) %
                                   kMaxRegisteredServices);
    }

    static uint64_t build_route_hash(std::string_view service_name,
                                   std::string_view method_name,
                                   RpcCallMode call_mode) {
        uint64_t hash = kFnvOffset;
        hash = hash_append(hash, service_name);
        hash ^= 0xffu;
        hash *= kFnvPrime;
        hash = hash_append(hash, method_name);
        hash ^= static_cast<uint64_t>(call_mode);
        hash *= kFnvPrime;
        return hash;
    }

    static bool route_cache_hit(const RouteCacheEntry& entry,
                              const RpcRequest& request,
                              uint64_t route_hash) {
        return entry.handler != nullptr &&
               entry.route_hash == route_hash &&
               entry.call_mode == request.call_mode() &&
               entry.service_name == request.service_name() &&
               entry.method_name == request.method_name();
    }

    static bool route_cache_key_match(const RouteCacheEntry& entry,
                                   const RpcRequest& request,
                                   uint64_t route_hash) {
        return entry.route_hash == route_hash &&
               entry.call_mode == request.call_mode() &&
               entry.service_name == request.service_name() &&
               entry.method_name == request.method_name();
    }

    static void assign_cached_string(std::string& cache_value, std::string_view incoming) {
        if (std::string_view(cache_value) == incoming) {
            return;
        }

        if (cache_value.capacity() < incoming.size()) {
            size_t target_capacity = cache_value.capacity() == 0 ? kRouteStringReserve : cache_value.capacity();
            while (target_capacity < incoming.size()) {
                target_capacity <<= 1;
            }
            cache_value.reserve(target_capacity);
        }

        cache_value.assign(incoming.data(), incoming.size());
    }

    RpcMethodHandler* find_cached_handler(const RpcRequest& request,
                                        uint64_t route_hash,
                                        std::array<RouteCacheEntry, kRouteCacheSize>& route_cache,
                                        RouteCacheEntry*& last_hit) const {
        if (last_hit != nullptr && route_cache_hit(*last_hit, request, route_hash)) {
            return last_hit->handler;
        }

        for (auto& entry : route_cache) {
            if (route_cache_hit(entry, request, route_hash)) {
                last_hit = &entry;
                return entry.handler;
            }
        }

        return nullptr;
    }

    void update_route_cache(const RpcRequest& request,
                          uint64_t route_hash,
                          RpcMethodHandler* handler,
                          std::array<RouteCacheEntry, kRouteCacheSize>& route_cache,
                          size_t& route_cache_cursor,
                          RouteCacheEntry*& last_hit) const {
        for (auto& entry : route_cache) {
            if (route_cache_key_match(entry, request, route_hash)) {
                entry.handler = handler;
                last_hit = &entry;
                return;
            }
        }

        auto& slot = route_cache[route_cache_cursor];
        route_cache_cursor = (route_cache_cursor + 1) % kRouteCacheSize;
        slot.route_hash = route_hash;
        assign_cached_string(slot.service_name, request.service_name());
        assign_cached_string(slot.method_name, request.method_name());
        slot.call_mode = request.call_mode();
        slot.handler = handler;
        last_hit = &slot;
    }

    std::expected<RpcMethodHandler*, RpcErrorCode> resolve_method_handler(const RpcRequest& request) {
        const size_t initial_index = service_bucket_index(request.service_name());
        for (size_t probe = 0; probe < m_services.size(); ++probe) {
            RpcService* service = m_services[(initial_index + probe) % m_services.size()];
            if (service == nullptr) {
                break;
            }
            if (service->name() == request.service_name()) {
                auto* handler = service->find_method(request.method_name(), request.call_mode());
                if (handler == nullptr) {
                    return std::unexpected(RpcErrorCode::METHOD_NOT_FOUND);
                }
                return handler;
            }
        }
        return std::unexpected(RpcErrorCode::SERVICE_NOT_FOUND);
    }

    /**
     * @brief 接受连接循环
     */
    Task<void> accept_loop(AsyncTcpSocket listener) {
        while (m_running.load(std::memory_order_acquire)) {
            Host client_host;
            auto accept_result = co_await listener.accept(&client_host);
            if (!accept_result) {
                m_last_error = RpcError::from(accept_result.error());
                RPC_LOG_WARN("[server] [accept] [fail]",
                             "error={}",
                             m_last_error->message());
                continue;
            }

            // 分发到下一个IO调度器处理
            auto* scheduler = m_runtime.get_next_io_scheduler();
            if (!schedule_task(scheduler, handle_connection(accept_result.value()))) {
                m_last_error = RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule connection handler");
                RPC_LOG_ERROR("[server] [schedule] [fail]", "connection-handler");
                GHandle accepted = accept_result.value();
                RpcConn conn(accepted, RpcReaderSetting{}, RpcWriterSetting{}, m_config.ring_buffer_size);
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                    RPC_LOG_WARN("[server] [close] [fail]",
                                 "error={}",
                                 m_last_error->message());
                }
            }
        }

        auto close_result = co_await listener.close();
        if (!close_result) {
            m_last_error = RpcError::from(close_result.error());
            RPC_LOG_WARN("[server] [listener] [close-fail]",
                         "error={}",
                         m_last_error->message());
        }
        co_return;
    }

    /**
     * @brief 处理连接
     */
    Task<void> handle_connection(GHandle handle) {
        RpcConn conn(handle, RpcReaderSetting{}, RpcWriterSetting{}, m_config.ring_buffer_size);
        if (m_config.tcp_no_delay) {
            auto nodelay_result = conn.socket().option().handle_tcp_no_delay();
            if (!nodelay_result) {
                RPC_LOG_WARN("[server] [socket] [nodelay-fail]",
                             "error={}",
                             nodelay_result.error().message());
            }
        }
        auto reader = conn.get_reader();
        auto writer = conn.get_writer();
        std::array<RouteCacheEntry, kRouteCacheSize> route_cache{};
        for (auto& entry : route_cache) {
            entry.service_name.reserve(kRouteStringReserve);
            entry.method_name.reserve(kRouteStringReserve);
        }
        size_t route_cache_cursor = 0;
        RouteCacheEntry* last_hit = nullptr;

        while (m_running.load(std::memory_order_acquire)) {
            // 读取请求（co_await直到完整消息）
            RpcRequest request;
            RpcHeader header;
            auto result = co_await GetRpcHeaderAwaitable<AsyncTcpSocket>(conn.ring_buffer(), header, conn.socket());
            if (!result) {
                // 错误，关闭连接
                m_last_error = result.error();
                RPC_LOG_WARN("[server] [recv] [fail]",
                             "code={} error={}",
                             static_cast<int>(m_last_error->code()),
                             m_last_error->message());
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                    RPC_LOG_WARN("[server] [close] [fail]",
                                 "error={}",
                                 m_last_error->message());
                }
                co_return;
            }

            if (header.m_type == static_cast<uint8_t>(RpcMessageType::HEARTBEAT)) {
                auto heartbeat_result = co_await SendRawDataAwaitable<AsyncTcpSocket>(
                    rpc_build_heartbeat_frame(header.m_request_id),
                    conn.socket());
                if (!heartbeat_result) {
                    m_last_error = heartbeat_result.error();
                    RPC_LOG_WARN("[server] [heartbeat] [send-fail]",
                                 "request_id={} code={} error={}",
                                 header.m_request_id,
                                 static_cast<int>(m_last_error->code()),
                                 m_last_error->message());
                    auto close_result = co_await conn.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                    }
                    co_return;
                }
                continue;
            }

            if (header.m_type != static_cast<uint8_t>(RpcMessageType::REQUEST)) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST, "Unexpected RPC message type");
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                }
                co_return;
            }

            if (header.m_body_length > RPC_MAX_BODY_SIZE) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST, "Message too large");
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                }
                co_return;
            }
            if ((header.m_reserved & static_cast<uint8_t>(~RPC_RESERVED_KNOWN_MASK)) != 0) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST, "Unsupported request reserved bits");
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                }
                co_return;
            }

            std::vector<char> request_body(header.m_body_length);
            if (header.m_body_length > 0) {
                result = co_await GetRpcBodyAwaitable<AsyncTcpSocket>(
                    conn.ring_buffer(),
                    request_body.data(),
                    request_body.size(),
                    conn.socket());
                if (!result) {
                    m_last_error = result.error();
                    auto close_result = co_await conn.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                    }
                    co_return;
                }
            }

            request.request_id(header.m_request_id);
            request.call_mode(rpc_decode_call_mode(header.m_flags));
            request.end_of_stream(rpc_is_end_stream(header.m_flags));
            if (!request.deserialize_body(request_body.data(),
                                         request_body.size(),
                                         (header.m_reserved & RPC_RESERVED_METADATA) != 0)) {
                m_last_error = RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Failed to parse request body");
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                }
                co_return;
            }

            // 处理请求
            RpcResponse response(request.request_id());
            response.call_mode(request.call_mode());
            response.end_of_stream(true);

            auto intercept_result = m_config.interceptor(request);
            if (!intercept_result.has_value()) {
                response.error_code(intercept_result.error().code());
                result = co_await writer.send_response(response);
                if (!result) {
                    m_last_error = result.error();
                    auto close_result = co_await conn.close();
                    (void)close_result;
                    co_return;
                }
                continue;
            }

            const uint64_t route_hash = build_route_hash(request.service_name(),
                                                       request.method_name(),
                                                       request.call_mode());
            RpcMethodHandler* handler = find_cached_handler(request,
                                                          route_hash,
                                                          route_cache,
                                                          last_hit);
            if (handler == nullptr) {
                auto resolve_result = resolve_method_handler(request);
                if (!resolve_result.has_value()) {
                    response.error_code(resolve_result.error());
                    RPC_LOG_WARN("[server] [route] [not-found]",
                                 "service={} method={} mode={} code={}",
                                 request.service_name(),
                                 request.method_name(),
                                 static_cast<int>(request.call_mode()),
                                 static_cast<int>(resolve_result.error()));
                } else {
                    handler = resolve_result.value();
                    update_route_cache(request,
                                     route_hash,
                                     handler,
                                     route_cache,
                                     route_cache_cursor,
                                     last_hit);
                }
            }

            if (handler != nullptr) {
                RpcContext ctx(request, response);
                co_await (*handler)(ctx);
            }

            response.materialize_payload();

            // 发送响应（co_await直到完整发送）
            result = co_await writer.send_response(response);
            if (!result) {
                m_last_error = result.error();
                RPC_LOG_WARN("[server] [send] [fail]",
                             "request_id={} code={} error={}",
                             response.request_id(),
                             static_cast<int>(m_last_error->code()),
                             m_last_error->message());
                auto close_result = co_await conn.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                    RPC_LOG_WARN("[server] [close] [fail]",
                                 "error={}",
                                 m_last_error->message());
                }
                co_return;
            }
        }

        auto close_result = co_await conn.close();
        if (!close_result) {
            m_last_error = RpcError::from(close_result.error());
            RPC_LOG_WARN("[server] [close] [fail]",
                         "error={}",
                         m_last_error->message());
        }
        co_return;
    }

private:
    RpcServerConfig m_config;          ///< 服务器配置
    Runtime m_runtime;                 ///< 运行时
    std::array<RpcService*, kMaxRegisteredServices> m_services{};  ///< 无所有权、无分配服务注册表
    std::optional<RpcError> m_last_error; ///< 最后一次错误
    std::atomic<bool> m_running{false}; ///< 运行标志
};

inline RpcServer RpcServerBuilder::build() const { return RpcServer(m_config); }

} // namespace galay::rpc

#endif // GALAY_RPC_SERVER_H
