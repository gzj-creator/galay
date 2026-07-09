/**
 * @file log_sink.h
 * @brief 日志输出 Sink 抽象基类
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义日志输出的抽象接口，用户可实现此接口将日志
 * 写入任意目标（控制台、文件、远程服务等）。
 */

#pragma once

#include "log_record.h"

namespace galay::tracing {

/**
 * @brief 日志输出 Sink 抽象基类
 * @details 定义日志记录的写入接口。实现类负责将 LogRecord
 * 格式化并输出到特定目标。
 */
class LogSink {
public:
    LogSink() = default;
    virtual ~LogSink() = default;

    /**
     * @brief 移动构造 Sink 基类状态
     */
    LogSink(LogSink&&) noexcept = default;

    /**
     * @brief 移动赋值 Sink 基类状态
     */
    LogSink& operator=(LogSink&&) noexcept = default;

    /**
     * @brief 写入一条日志记录
     * @param record 日志记录
     */
    virtual void write(const LogRecord& record) = 0;

private:
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
};

} // namespace galay::tracing
