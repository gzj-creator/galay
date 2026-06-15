/**
 * @file streamsvc.h
 * @brief 真实流式 RPC 服务器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 基于 STREAM_INIT/STREAM_DATA/STREAM_END 协议，
 *          按 service/method 路由到 RpcService::registerStreamMethod 注册的回调。
 */

#ifndef GALAY_RPC_STREAMSVC_H
#define GALAY_RPC_STREAMSVC_H

#include "galay-rpc/common/rpc_log.h"
#include "rpc_service.h"
#include "rpc_stream.h"
#include "galay-rpc/utils/runtime_compat.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/core/runtime.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;

/**
 * @brief 流式 RPC 服务器配置
 */
struct RpcStreamServerConfig {
    std::string host = "0.0.0.0";              ///< 监听地址
    uint16_t port = 9100;                      ///< 监听端口
    int backlog = 1024;                        ///< 监听队列长度
    size_t io_scheduler_count = 0;             ///< IO调度器数量，0表示自动
    size_t compute_scheduler_count = 0;        ///< 计算调度器数量，0表示自动
    RuntimeAffinityConfig affinity;            ///< 绑核配置
    size_t ring_buffer_size = 128 * 1024;      ///< RingBuffer大小
};

class RpcStreamServer;

/**
 * @brief 流式RPC服务器构建器
 *
 * @details 使用Builder模式配置并创建RpcStreamServer实例。
 */
class RpcStreamServerBuilder {
public:
    /// @brief 设置监听地址
    RpcStreamServerBuilder& host(std::string value)                         { m_config.host = std::move(value); return *this; }
    /// @brief 设置监听端口
    RpcStreamServerBuilder& port(uint16_t value)                            { m_config.port = value; return *this; }
    /// @brief 设置监听队列长度
    RpcStreamServerBuilder& backlog(int value)                              { m_config.backlog = value; return *this; }
    /// @brief 设置IO调度器数量
    RpcStreamServerBuilder& ioSchedulerCount(size_t value)                  { m_config.io_scheduler_count = value; return *this; }
    /// @brief 设置计算调度器数量
    RpcStreamServerBuilder& computeSchedulerCount(size_t value)             { m_config.compute_scheduler_count = value; return *this; }
    /**
     * @brief 设置顺序绑核策略
     * @param io_count IO调度器绑核数
     * @param compute_count 计算调度器绑核数
     * @return 构建器引用
     */
    RpcStreamServerBuilder& sequentialAffinity(size_t io_count, size_t compute_count) {
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
    RpcStreamServerBuilder& ringBufferSize(size_t value)                    { m_config.ring_buffer_size = value; return *this; }
    /// @brief 构建RpcStreamServer实例
    RpcStreamServer build() const;
    /// @brief 仅导出配置
    RpcStreamServerConfig buildConfig() const                               { return m_config; }

private:
    RpcStreamServerConfig m_config;  ///< 服务器配置
};

/**
 * @brief 真实流式 RPC 服务器
 *
 * @note 当前每条连接同一时刻只处理一个活跃流会话（顺序处理）。
 */
class RpcStreamServer {
public:
    /**
     * @brief 构造流式RPC服务器
     * @param config 服务器配置
     */
    explicit RpcStreamServer(const RpcStreamServerConfig& config)
        : m_config(config)
        , m_runtime(RuntimeBuilder()
                        .ioSchedulerCount(resolveIoSchedulerCount(config.io_scheduler_count))
                        .computeSchedulerCount(config.compute_scheduler_count)
                        .applyAffinity(config.affinity)
                        .build()) {}

    ~RpcStreamServer() {
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
        RPC_LOG_INFO("[stream-server] [start]",
                     "host={} port={} backlog={}",
                     m_config.host,
                     m_config.port,
                     m_config.backlog);
        m_running.store(true, std::memory_order_release);
        m_runtime.start();
        auto* scheduler = m_runtime.getNextIOScheduler();
        if (!scheduleTask(scheduler, acceptLoop())) {
            m_last_error = RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule stream accept loop");
            RPC_LOG_ERROR("[stream-server] [schedule] [fail]", "accept-loop");
            m_running.store(false, std::memory_order_release);
            m_runtime.stop();
        }
    }

    /**
     * @brief 停止服务器
     */
    void stop() {
        if (m_running.exchange(false, std::memory_order_acq_rel)) {
            RPC_LOG_INFO("[stream-server] [stop]", "port={}", m_config.port);
            m_runtime.stop();
        }
    }

    /// @brief 检查是否运行中
    bool isRunning() const {
        return m_running.load(std::memory_order_acquire);
    }

    /// @brief 获取Runtime
    Runtime& runtime() { return m_runtime; }

    /// @brief 获取最近一次服务器错误
    std::optional<RpcError> lastError() const {
        return m_last_error;
    }

private:
    std::expected<RpcStreamHandler*, RpcErrorCode> resolveStreamHandler(const StreamInitRequest& init_req) {
        auto service_it = m_services.find(init_req.serviceName());
        if (service_it == m_services.end()) {
            return std::unexpected(RpcErrorCode::SERVICE_NOT_FOUND);
        }

        auto* handler = service_it->second->findStreamMethod(init_req.methodName());
        if (handler == nullptr) {
            return std::unexpected(RpcErrorCode::METHOD_NOT_FOUND);
        }

        return handler;
    }

    Coroutine acceptLoop() {
        m_last_error.reset();

        TcpSocket listener(IPType::IPV4);
        auto reuse_addr_result = listener.option().handleReuseAddr();
        if (!reuse_addr_result) {
            m_last_error = RpcError::from(reuse_addr_result.error());
            RPC_LOG_ERROR("[stream-server] [socket] [reuseaddr-fail]",
                          "error={}",
                          m_last_error->message());
            co_return;
        }

        auto non_block_result = listener.option().handleNonBlock();
        if (!non_block_result) {
            m_last_error = RpcError::from(non_block_result.error());
            RPC_LOG_ERROR("[stream-server] [socket] [nonblock-fail]",
                          "error={}",
                          m_last_error->message());
            co_return;
        }

        Host host(IPType::IPV4, m_config.host, m_config.port);
        auto bind_result = listener.bind(host);
        if (!bind_result) {
            m_last_error = RpcError::from(bind_result.error());
            RPC_LOG_ERROR("[stream-server] [bind] [fail]",
                          "host={} port={} error={}",
                          m_config.host,
                          m_config.port,
                          m_last_error->message());
            co_return;
        }

        auto listen_result = listener.listen(m_config.backlog);
        if (!listen_result) {
            m_last_error = RpcError::from(listen_result.error());
            RPC_LOG_ERROR("[stream-server] [listen] [fail]",
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
                RPC_LOG_WARN("[stream-server] [accept] [fail]",
                             "error={}",
                             m_last_error->message());
                continue;
            }

            auto* scheduler = m_runtime.getNextIOScheduler();
            if (!scheduleTask(scheduler, handleConnection(accept_result.value()))) {
                m_last_error = RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule stream connection handler");
                RPC_LOG_ERROR("[stream-server] [schedule] [fail]", "connection-handler");
                TcpSocket socket(accept_result.value());
                auto close_result = co_await socket.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                    RPC_LOG_WARN("[stream-server] [close] [fail]",
                                 "error={}",
                                 m_last_error->message());
                }
            }
        }

        auto close_result = co_await listener.close();
        if (!close_result) {
            m_last_error = RpcError::from(close_result.error());
            RPC_LOG_WARN("[stream-server] [listener] [close-fail]",
                         "error={}",
                         m_last_error->message());
        }
        co_return;
    }

    Coroutine handleConnection(GHandle handle) {
        TcpSocket socket(handle);
        auto non_block_result = socket.option().handleNonBlock();
        if (!non_block_result) {
            m_last_error = RpcError::from(non_block_result.error());
            RPC_LOG_ERROR("[stream-server] [socket] [client-nonblock-fail]",
                          "error={}",
                          m_last_error->message());
            auto close_result = co_await socket.close();
            if (!close_result) {
                m_last_error = RpcError::from(close_result.error());
                RPC_LOG_WARN("[stream-server] [close] [fail]",
                             "error={}",
                             m_last_error->message());
            }
            co_return;
        }

        RingBuffer ring_buffer(m_config.ring_buffer_size == 0 ? 128 * 1024 : m_config.ring_buffer_size);
        StreamReader reader(ring_buffer, socket);

        while (m_running.load(std::memory_order_acquire)) {
            StreamMessage init_frame;
            auto recv_result = co_await reader.getMessage(init_frame);
            if (!recv_result.has_value()) {
                m_last_error = recv_result.error();
                RPC_LOG_WARN("[stream-server] [recv] [fail]",
                             "code={} error={}",
                             static_cast<int>(m_last_error->code()),
                             m_last_error->message());
                auto close_result = co_await socket.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                    RPC_LOG_WARN("[stream-server] [close] [fail]",
                                 "error={}",
                                 m_last_error->message());
                }
                co_return;
            }

            const uint32_t stream_id = init_frame.streamId();
            RpcStream stream(socket, ring_buffer, stream_id);

            if (init_frame.messageType() != RpcMessageType::STREAM_INIT) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST,
                                        "Expected STREAM_INIT as first frame");
                RPC_LOG_WARN("[stream-server] [protocol] [invalid-first-frame]",
                             "stream_id={} type={}",
                             stream_id,
                             static_cast<int>(init_frame.messageType()));
                auto cancel_result = co_await stream.sendCancel();
                if (!cancel_result.has_value()) {
                    m_last_error = cancel_result.error();
                    RPC_LOG_WARN("[stream-server] [cancel] [fail]",
                                 "stream_id={} error={}",
                                 stream_id,
                                 m_last_error->message());
                    auto close_result = co_await socket.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                        RPC_LOG_WARN("[stream-server] [close] [fail]",
                                     "error={}",
                                     m_last_error->message());
                    }
                    co_return;
                }
                continue;
            }

            StreamInitRequest init_req;
            if (!init_req.deserializeBody(init_frame.payload().data(), init_frame.payload().size())) {
                m_last_error = RpcError(RpcErrorCode::INVALID_REQUEST,
                                        "Failed to parse stream init body");
                RPC_LOG_WARN("[stream-server] [protocol] [parse-init-fail]",
                             "stream_id={} payload_size={}",
                             stream_id,
                             init_frame.payload().size());
                auto cancel_result = co_await stream.sendCancel();
                if (!cancel_result.has_value()) {
                    m_last_error = cancel_result.error();
                    RPC_LOG_WARN("[stream-server] [cancel] [fail]",
                                 "stream_id={} error={}",
                                 stream_id,
                                 m_last_error->message());
                    auto close_result = co_await socket.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                        RPC_LOG_WARN("[stream-server] [close] [fail]",
                                     "error={}",
                                     m_last_error->message());
                    }
                    co_return;
                }
                continue;
            }

            auto handler_result = resolveStreamHandler(init_req);
            if (!handler_result.has_value()) {
                m_last_error = RpcError(handler_result.error());
                RPC_LOG_WARN("[stream-server] [route] [not-found]",
                             "stream_id={} service={} method={} code={}",
                             stream_id,
                             init_req.serviceName(),
                             init_req.methodName(),
                             static_cast<int>(handler_result.error()));
                auto cancel_result = co_await stream.sendCancel();
                if (!cancel_result.has_value()) {
                    m_last_error = cancel_result.error();
                    RPC_LOG_WARN("[stream-server] [cancel] [fail]",
                                 "stream_id={} error={}",
                                 stream_id,
                                 m_last_error->message());
                    auto close_result = co_await socket.close();
                    if (!close_result) {
                        m_last_error = RpcError::from(close_result.error());
                        RPC_LOG_WARN("[stream-server] [close] [fail]",
                                     "error={}",
                                     m_last_error->message());
                    }
                    co_return;
                }
                continue;
            }

            stream.setRoute(init_req.serviceName(), init_req.methodName());

            auto send_result = co_await stream.sendInitAck();
            if (!send_result.has_value()) {
                m_last_error = send_result.error();
                RPC_LOG_WARN("[stream-server] [send-init-ack] [fail]",
                             "stream_id={} error={}",
                             stream_id,
                             m_last_error->message());
                auto close_result = co_await socket.close();
                if (!close_result) {
                    m_last_error = RpcError::from(close_result.error());
                    RPC_LOG_WARN("[stream-server] [close] [fail]",
                                 "error={}",
                                 m_last_error->message());
                }
                co_return;
            }

            auto* handler = handler_result.value();
            co_await (*handler)(stream);
        }

        auto close_result = co_await socket.close();
        if (!close_result) {
            m_last_error = RpcError::from(close_result.error());
            RPC_LOG_WARN("[stream-server] [close] [fail]",
                         "error={}",
                         m_last_error->message());
        }
        co_return;
    }

private:
    RpcStreamServerConfig m_config;         ///< 服务器配置
    Runtime m_runtime;                      ///< 运行时
    std::unordered_map<std::string, std::shared_ptr<RpcService>> m_services;  ///< 服务注册表
    std::atomic<bool> m_running{false};     ///< 运行标志
    std::optional<RpcError> m_last_error;   ///< 最后一次错误
};

inline RpcStreamServer RpcStreamServerBuilder::build() const { return RpcStreamServer(m_config); }

} // namespace galay::rpc

#endif // GALAY_RPC_STREAMSVC_H
