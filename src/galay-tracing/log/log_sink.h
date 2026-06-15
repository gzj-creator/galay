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

#include "galay-tracing/log/log_record.h"

namespace galay::tracing {

/**
 * @brief 日志输出 Sink 抽象基类
 * @details 定义日志记录的写入接口。实现类负责将 LogRecord
 * 格式化并输出到特定目标。
 */
class LogSink {
public:
    virtual ~LogSink() = default;

    /**
     * @brief 写入一条日志记录
     * @param record 日志记录
     */
    virtual void write(const LogRecord& record) = 0;
};

} // namespace galay::tracing
