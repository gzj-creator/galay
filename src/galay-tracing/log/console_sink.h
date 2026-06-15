/**
 * @file console_sink.h
 * @brief 控制台日志输出 Sink
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 将日志记录格式化并输出到标准输出流（std::cout 或 std::cerr），
 * 适用于开发和调试场景。
 */

#pragma once

#include "galay-tracing/log/log_sink.h"

#include <iosfwd>

namespace galay::tracing {

/**
 * @brief 控制台日志输出 Sink
 * @details 将 LogRecord 格式化输出到指定的输出流（默认为 std::cout）。
 */
class ConsoleSink final : public LogSink {
public:
    /**
     * @brief 构造控制台 Sink，使用指定的输出流
     * @param out 输出流引用
     */
    explicit ConsoleSink(std::ostream& out);

    /**
     * @brief 构造控制台 Sink，使用默认的 std::cout
     */
    ConsoleSink();

    /**
     * @brief 将日志记录写入控制台
     * @param record 日志记录
     */
    void write(const LogRecord& record) override;

private:
    std::ostream* m_out; ///< 输出流指针
};

} // namespace galay::tracing
