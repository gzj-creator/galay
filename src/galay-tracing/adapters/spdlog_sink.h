/**
 * @file spdlog_sink.h
 * @brief 基于 spdlog 的日志输出适配器
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 将 galay-tracing 的 LogRecord 适配到 spdlog 日志框架，
 * 使追踪日志能够通过 spdlog 的管道输出。
 */

#pragma once

#include "galay-tracing/log/log_sink.h"

#include <memory>

namespace spdlog {
class logger;
} // namespace spdlog

namespace galay::tracing {

/**
 * @brief 基于 spdlog 的日志 Sink 实现
 * @details 将 LogRecord 转发到 spdlog::logger 进行输出，支持 spdlog 的
 * 所有格式化和输出目标（文件、控制台、旋转文件等）。
 */
class SpdlogSink final : public LogSink {
public:
    /**
     * @brief 构造 spdlog 日志 Sink
     * @param logger spdlog logger 实例的共享指针
     */
    explicit SpdlogSink(std::shared_ptr<spdlog::logger> logger);

    /**
     * @brief 将日志记录写入 spdlog
     * @param record 日志记录
     */
    void write(const LogRecord& record) override;

private:
    std::shared_ptr<spdlog::logger> m_logger; ///< spdlog logger 实例
};

} // namespace galay::tracing
