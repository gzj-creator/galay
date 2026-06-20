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
 * auto echoService = std::make_shared<EchoService>();
 *
 * // 启动服务器
 * auto server = RpcServerBuilder()
 *     .host("0.0.0.0")
 *     .port(9000)
 *     .build();
 * server.registerService(echoService);
 * server.start();
 * @endcode
 */

#ifndef GALAY_RPC_SERVER_H
#define GALAY_RPC_SERVER_H

#include "../common/rpc_log.h"
#include "rpc_service.h"
#include "rpc_conn.h"
#include "../utils/runtime_compat.h"
#include "../../galay-kernel/core/runtime.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
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
    size_t io_scheduler_count = 0;      ///< IO调度器数量，0表示自动
    size_t compute_scheduler_count = 0; ///< 计算调度器数量，0表示自动
    RuntimeAffinityConfig affinity;     ///< 绑核配置
    size_t ring_buffer_size = kDefaultRpcRingBufferSize;  ///< RingBuffer大小
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
    /// @brief 设置IO调度器数量
    RpcServerBuilder& ioSchedulerCount(size_t value)                     { m_config.io_scheduler_count = value; return *this; }
    /// @brief 设置计算调度器数量
    RpcServerBuilder& computeSchedulerCount(size_t value)                { m_config.compute_scheduler_count = value; return *this; }
    /**
     * @brief 设置顺序绑核策略
     * @param io_count IO调度器绑核数
     * @param compute_count 计算调度器绑核数
     * @return 构建器引用
     */
    RpcServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count) {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = io_count;
        m_config.affinity.seq_compute_count = compute_count;
        return *this;
    }
    /**
     * @brief 设置自定义绑核策略
     * @param io_cpus IO调度器绑定的CPU列表
     * @param compute_cpus 计算调度器绑定的CPU列表
     * @return 是否设置成功（数量必须匹配调度器数量）
     */
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
    /// @brief 设置环形缓冲区大小
    RpcServerBuilder& ringBufferSize(size_t value)                       { m_config.ring_buffer_size = value; return *this; }
    /// @brief 构建RpcServer实例
    RpcServer build() const;
    /// @brief 仅导出配置
    RpcServerConfig buildConfig() const                                  { return m_config; }

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
    /**
     * @brief 构造函数
     * @param config 服务器配置
     */
    explicit RpcServer(const RpcServerConfig& config)
        : m_config(config)
        , m_runtime(RuntimeBuilder()
                        .ioSchedulerCount(resolveIoSchedulerCount(config.io_scheduler_count))
                        .computeSchedulerCount(config.compute_scheduler_count)
                        .applyAffinity(config.affinity)
                        .build()) {}

    ~RpcServer() {
        stop();
    }

    /**
     * @brief 注册服务
     * @param service 服务实例
     */
    void registerService(std::shared_ptr<RpcService> service) {
        m_services[service->name()] = std::move(service);
    }

    /**
     * @brief 启动服务器
     */
    void start() {
        RPC_LOG_INFO("[server] [start]",
                     "host={} port={} backlog={}",
                     m_config.host,
                     m_config.port,
                     m_config.backlog);
        m_running.store(true, std::memory_order_release);
        m_runtime.start();

        auto* scheduler = m_runtime.getNextIOScheduler();
        if (!scheduleTask(scheduler, acceptLoop())) {
            m_last_error = RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule accept loop");
            RPC_LOG_ERROR("[server] [schedule] [fail]", "accept-loop");
            m_running.store(false, std::memory_order_release);
            m_runtime.stop();
        }
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
    bool isRunning() const {
        return m_running.load(std::memory_order_acquire);
    }

    /**
     * @brief 获取Runtime
     */
    Runtime& runtime() { return m_runtime; }

    /**
     * @brief 获取最近一次服务器错误（若有）
     * @note 非线程安全：仅用于单线程启动阶段查询错误。
     */
    std::optional<RpcError> lastError() const {
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

    static uint64_t hashAppend(uint64_t hash, std::string_view str) {
        for (unsigned char ch : str) {
            hash ^= static_cast<uint64_t>(ch);
            hash *= kFnvPrime;
        }
        return hash;
    }

    static uint64_t buildRouteHash(std::string_view service_name,
                                   std::string_view method_name,
                                   RpcCallMode call_mode) {
        uint64_t hash = kFnvOffset;
        hash = hashAppend(hash, service_name);
        hash ^= 0xffu;
        hash *= kFnvPrime;
        hash = hashAppend(hash, method_name);
        hash ^= static_cast<uint64_t>(call_mode);
        hash *= kFnvPrime;
        return hash;
    }

    static bool routeCacheHit(const RouteCacheEntry& entry,
                              const RpcRequest& request,
                              uint64_t route_hash) {
        return entry.handler != nullptr &&
               entry.route_hash == route_hash &&
               entry.call_mode == request.callMode() &&
               entry.service_name == request.serviceName() &&
               entry.method_name == request.methodName();
    }

    static bool routeCacheKeyMatch(const RouteCacheEntry& entry,
                                   const RpcRequest& request,
                                   uint64_t route_hash) {
        return entry.route_hash == route_hash &&
               entry.call_mode == request.callMode() &&
               entry.service_name == request.serviceName() &&
               entry.method_name == request.methodName();
    }

    static void assignCachedString(std::string& cache_value, std::string_view incoming) {
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

    RpcMethodHandler* findCachedHandler(const RpcRequest& request,
                                        uint64_t route_hash,
                                        std::array<RouteCacheEntry, kRouteCacheSize>& route_cache,
                                        RouteCacheEntry*& last_hit) const {
        if (last_hit != nullptr && routeCacheHit(*last_hit, request, route_hash)) {
            return last_hit->handler;
        }

        for (auto& entry : route_cache) {
            if (routeCacheHit(entry, request, route_hash)) {
                last_hit = &entry;
                return entry.handler;
            }
        }

        return nullptr;
    }

    void updateRouteCache(const RpcRequest& request,
                          uint64_t route_hash,
                          RpcMethodHandler* handler,
                          std::array<RouteCacheEntry, kRouteCacheSize>& route_cache,
                          size_t& route_cache_cursor,
                          RouteCacheEntry*& last_hit) const {
        for (auto& entry : route_cache) {
            if (routeCacheKeyMatch(entry, request, route_hash)) {
                entry.handler = handler;
                last_hit = &entry;
                return;
            }
        }

        auto& slot = route_cache[route_cache_cursor];
        route_cache_cursor = (route_cache_cursor + 1) % kRouteCacheSize;
        slot.route_hash = route_hash;
        assignCachedString(slot.service_name, request.serviceName());
        assignCachedString(slot.method_name, request.methodName());
        slot.call_mode = request.callMode();
        slot.handler = handler;
        last_hit = &slot;
    }

    std::expected<RpcMethodHandler*, RpcErrorCode> resolveMethodHandler(const RpcRequest& request) {
        auto service_it = m_services.find(request.serviceName());
        if (service_it == m_services.end()) {
            return std::unexpected(RpcErrorCode::SERVICE_NOT_FOUND);
        }

        auto& service = service_it->second;
        auto* handler = service->findMethod(request.methodName(), request.callMode());
        if (handler == nullptr) {
            return std::unexpected(RpcErrorCode::METHOD_NOT_FOUND);
        }

        return handler;
    }

    /**
     * @brief 接受连接循环
     */
    Task<void> acceptLoop() {
        m_last_error.reset();

        TcpSocket listener(IPType::IPV4);
        auto reuse_addr_result = listener.option().handleReuseAddr();
        if (!reuse_addr_result) {
            m_last_error = RpcError::from(reuse_addr_result.error());
            RPC_LOG_ERROR("[server] [socket] [reuseaddr-fail]",
                          "error={}",
                          m_last_error->message());
            co_return;
        }
        auto non_block_result = listener.option().handleNonBlock();
        if (!non_block_result) {
            m_last_error = RpcError::from(non_block_result.error());
            RPC_LOG_ERROR("[server] [socket] [nonblock-fail]",
                          "error={}",
                          m_last_error->message());
            co_return;
        }

        Host host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(host);
        if (!bind_result) {
            m_last_error = RpcError::from(bind_result.error());
            RPC_LOG_ERROR("[server] [bind] [fail]",
                          "host={} port={} error={}",
                          m_config.host,
                          m_config.port,
                          m_last_error->message());
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            m_last_error = RpcError::from(listen_result.error());
            RPC_LOG_ERROR("[server] [listen] [fail]",
                          "port={} backlog={} error={}",
                          m_config.port,
                          m_config.backlog,
                          m_last_error->message());
            co_return;
        }

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
            auto* scheduler = m_runtime.getNextIOScheduler();
            if (!scheduleTask(scheduler, handleConnection(accept_result.value()))) {
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
    Task<void> handleConnection(GHandle handle) {
        RpcConn conn(handle, RpcReaderSetting{}, RpcWriterSetting{}, m_config.ring_buffer_size);
        auto reader = conn.getReader();
        auto writer = conn.getWriter();
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
            auto result = co_await reader.getRequest(request);
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

            // 处理请求
            RpcResponse response(request.requestId());
            response.callMode(request.callMode());
            response.endOfStream(true);

            const uint64_t route_hash = buildRouteHash(request.serviceName(),
                                                       request.methodName(),
                                                       request.callMode());
            RpcMethodHandler* handler = findCachedHandler(request,
                                                          route_hash,
                                                          route_cache,
                                                          last_hit);
            if (handler == nullptr) {
                auto resolve_result = resolveMethodHandler(request);
                if (!resolve_result.has_value()) {
                    response.errorCode(resolve_result.error());
                    RPC_LOG_WARN("[server] [route] [not-found]",
                                 "service={} method={} mode={} code={}",
                                 request.serviceName(),
                                 request.methodName(),
                                 static_cast<int>(request.callMode()),
                                 static_cast<int>(resolve_result.error()));
                } else {
                    handler = resolve_result.value();
                    updateRouteCache(request,
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

            // 发送响应（co_await直到完整发送）
            result = co_await writer.sendResponse(response);
            if (!result) {
                m_last_error = result.error();
                RPC_LOG_WARN("[server] [send] [fail]",
                             "request_id={} code={} error={}",
                             response.requestId(),
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
    std::unordered_map<std::string, std::shared_ptr<RpcService>> m_services;  ///< 服务注册表
    std::atomic<bool> m_running{false}; ///< 运行标志
    std::optional<RpcError> m_last_error; ///< 最后一次错误
};

inline RpcServer RpcServerBuilder::build() const { return RpcServer(m_config); }

} // namespace galay::rpc

#endif // GALAY_RPC_SERVER_H
