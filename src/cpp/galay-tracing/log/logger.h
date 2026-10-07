/**
 * @file logger.h
 * @brief 日志系统核心：Logger、Writer 概念、上下文日志代理与全局 API
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 提供 galay-tracing 的完整日志基础设施，包括：
 * - LogWriter / StructuredLogWriter 编译期概念约束
 * - 类型擦除的 ErasedLogWriter 和默认/借用 Writer 实现
 * - Logger 类（基于 Sink 的日志器）
 * - ContextLogger / ContextEventLogger 上下文绑定代理
 * - log() / event() 工厂函数
 * - 各级别便捷日志函数和宏
 */

#pragma once

#include "../common/source_location.h"
#include "../context/context_storage.h"
#include "log_level.h"
#include "log_record.h"
#include "log_sink.h"

#include <atomic>
#include <array>
#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace galay::tracing {

/**
 * @brief 日志写入器的编译期概念约束
 * @details 要求 Writer 类型支持 is_enabled(level) 和 write(record) 操作。
 * @tparam Writer 待检查的写入器类型
 */
template <typename Writer>
concept LogWriter = requires(Writer& writer, const Writer& constWriter, LogLevel level, LogRecord record) {
    { constWriter.is_enabled(level) } noexcept -> std::convertible_to<bool>;
    writer.write(std::move(record));
};

/**
 * @brief 结构化日志写入器的编译期概念约束
 * @details 要求 Writer 类型支持 is_enabled(level) 和 write(structuredRecord) 操作。
 * @tparam Writer 待检查的写入器类型
 */
template <typename Writer>
concept StructuredLogWriter = requires(Writer& writer, const Writer& constWriter, LogLevel level, StructuredLogRecord record) {
    { constWriter.is_enabled(level) } noexcept -> std::convertible_to<bool>;
    writer.write(record);
};

namespace detail {

/**
 * @brief 类型擦除的日志写入器
 * @details 通过函数指针擦除具体 Writer 类型，支持普通日志和结构化日志的写入。
 * 对象指针和函数指针共同构成完整的写入器接口。
 */
struct ErasedLogWriter {
    using IsEnabledFn = bool (*)(const void*, LogLevel) noexcept;       ///< 级别检查函数指针类型
    using WriteFn = void (*)(void*, LogRecord);                         ///< 普通日志写入函数指针类型
    using WriteStructuredFn = void (*)(void*, StructuredLogRecord);     ///< 结构化日志写入函数指针类型

    void* object = nullptr;                  ///< 擦除类型后的 Writer 对象指针
    IsEnabledFn is_enabled_fn = nullptr;       ///< 级别检查函数指针
    WriteFn write_fn = nullptr;               ///< 普通写入函数指针
    WriteStructuredFn write_structured_fn = nullptr; ///< 结构化写入函数指针

    /**
     * @brief 检查给定级别是否启用
     * @param level 日志级别
     * @return 启用返回 true
     */
    [[nodiscard]] bool is_enabled(LogLevel level) const noexcept {
        return object != nullptr && is_enabled_fn != nullptr && is_enabled_fn(object, level);
    }

    /**
     * @brief 写入普通日志记录
     * @details 优先使用 write_fn，若不可用则回退到结构化写入（无字段）
     * @param record 日志记录
     */
    void write(LogRecord record) const {
        if (object == nullptr) {
            return;
        }
        if (write_fn != nullptr) {
            write_fn(object, std::move(record));
            return;
        }
        if (write_structured_fn != nullptr) {
            write_structured_fn(object, StructuredLogRecord{
                .level = record.level,
                .name = record.message,
                .fields = {},
                .source = record.source,
                .context = std::move(record.context),
            });
        }
    }

    /**
     * @brief 写入结构化日志记录
     * @details 优先使用 write_structured_fn，若不可用则回退到普通写入
     * @param record 结构化日志记录
     */
    void write(StructuredLogRecord record) const {
        if (object == nullptr) {
            return;
        }
        if (write_structured_fn != nullptr) {
            write_structured_fn(object, std::move(record));
            return;
        }
        write_structured_fallback(std::move(record));
    }

    /**
     * @brief 结构化日志的普通写入回退实现
     * @param record 结构化日志记录
     */
    void write_structured_fallback(StructuredLogRecord record) const;
};

/**
 * @brief 默认日志写入器
 * @details 持有 ErasedLogWriter 指针的轻量级值类型，满足 LogWriter 和 StructuredLogWriter 概念。
 */
class DefaultLogWriter {
public:
    /**
     * @brief 构造默认写入器
     * @param writer 类型擦除写入器指针（可为 nullptr）
     */
    explicit constexpr DefaultLogWriter(const ErasedLogWriter* writer = nullptr) noexcept
        : m_writer(writer) {
    }

    /**
     * @brief 检查级别是否启用
     */
    [[nodiscard]] bool is_enabled(LogLevel level) const noexcept {
        return m_writer != nullptr && m_writer->is_enabled(level);
    }

    /**
     * @brief 写入普通日志记录
     */
    void write(LogRecord record) const {
        if (m_writer != nullptr) {
            m_writer->write(std::move(record));
        }
    }

    /**
     * @brief 写入结构化日志记录
     */
    void write(StructuredLogRecord record) const {
        if (m_writer != nullptr) {
            m_writer->write(record);
        }
    }

private:
    const ErasedLogWriter* m_writer; ///< 类型擦除写入器指针
};

/**
 * @brief 借用外部 Writer 的包装器
 * @details 持有外部 Writer 的指针，不获取所有权。满足 LogWriter 或 StructuredLogWriter 概念。
 * @tparam Writer 外部 Writer 类型
 */
template <typename Writer>
    requires(LogWriter<Writer> || StructuredLogWriter<Writer>)
class BorrowedLogWriter {
public:
    /**
     * @brief 构造借用写入器
     * @param writer 外部 Writer 引用
     */
    explicit BorrowedLogWriter(Writer& writer) noexcept
        : m_writer(&writer) {
    }

    /**
     * @brief 检查级别是否启用
     */
    [[nodiscard]] bool is_enabled(LogLevel level) const noexcept {
        return m_writer != nullptr && m_writer->is_enabled(level);
    }

    /**
     * @brief 写入普通日志记录（仅当 Writer 满足 LogWriter 概念时可用）
     */
    void write(LogRecord record)
        requires LogWriter<Writer> {
        if (m_writer != nullptr) {
            m_writer->write(std::move(record));
        }
    }

    /**
     * @brief 写入结构化日志记录（仅当 Writer 满足 StructuredLogWriter 概念时可用）
     */
    void write(StructuredLogRecord record)
        requires StructuredLogWriter<Writer> {
        if (m_writer != nullptr) {
            m_writer->write(record);
        }
    }

private:
    Writer* m_writer; ///< 外部 Writer 指针
};

/**
 * @brief 获取 Writer 类型的普通写入函数指针
 * @tparam Writer 满足 LogWriter 或 StructuredLogWriter 概念的类型
 * @return 写入函数指针，不满足 LogWriter 时返回 nullptr
 */
template <typename Writer>
[[nodiscard]] constexpr ErasedLogWriter::WriteFn log_write_fn() noexcept {
    if constexpr (LogWriter<Writer>) {
        return [](void* object, LogRecord record) {
            static_cast<Writer*>(object)->write(std::move(record));
        };
    } else {
        return nullptr;
    }
}

/**
 * @brief 获取 Writer 类型的结构化写入函数指针
 * @tparam Writer 满足 StructuredLogWriter 概念的类型
 * @return 结构化写入函数指针，不满足时返回 nullptr
 */
template <typename Writer>
[[nodiscard]] constexpr ErasedLogWriter::WriteStructuredFn structured_log_write_fn() noexcept {
    if constexpr (StructuredLogWriter<Writer>) {
        return [](void* object, StructuredLogRecord record) {
            static_cast<Writer*>(object)->write(record);
        };
    } else {
        return nullptr;
    }
}

/**
 * @brief 未经级别检查的结构化事件写入
 * @details 内部使用，由宏在级别检查通过后调用
 * @tparam kLevel 日志级别
 * @tparam Writer 结构化日志写入器类型
 * @tparam Fields 字段类型列表
 * @param writer 写入器引用
 * @param context 追踪上下文
 * @param source 源码位置
 * @param name 事件名称
 * @param fields 结构化字段列表
 */
template <LogLevel kLevel, StructuredLogWriter Writer, typename... Fields>
void write_event_unchecked(
    Writer& writer,
    std::optional<TraceContext> context,
    SourceLocation source,
    std::string_view name,
    Fields&&... fields) {
    std::array<LogField, sizeof...(Fields)> fieldArray{std::forward<Fields>(fields)...};
    writer.write(StructuredLogRecord{
        .level = kLevel,
        .name = name,
        .fields = std::span<const LogField>(fieldArray.data(), fieldArray.size()),
        .source = source,
        .context = make_log_context(std::move(context)),
    });
}

void set_default_log_writer_ref(ErasedLogWriter writer) noexcept;       ///< 设置默认写入器引用
extern std::atomic<const ErasedLogWriter*> g_defaultLogWriterPtr;   ///< 全局默认写入器原子指针

/**
 * @brief 获取内置默认写入器指针
 * @return 内置默认写入器指针
 */
[[nodiscard]] const ErasedLogWriter* built_in_default_log_writer_ptr() noexcept;

/**
 * @brief 获取当前默认写入器指针
 * @return 优先返回用户设置的写入器，否则返回内置默认写入器
 */
[[nodiscard]] inline const ErasedLogWriter* default_log_writer_ptr() noexcept {
    if (auto* writer = g_defaultLogWriterPtr.load(std::memory_order_acquire); writer != nullptr) {
        return writer;
    }
    return built_in_default_log_writer_ptr();
}

/**
 * @brief 获取默认写入器的类型擦除引用
 */
[[nodiscard]] ErasedLogWriter default_log_writer_ref() noexcept;

/**
 * @brief 获取默认写入器的值类型实例
 */
[[nodiscard]] DefaultLogWriter default_log_writer() noexcept;

} // namespace detail

/**
 * @brief 基于 Sink 的日志器
 * @details 管理多个 LogSink 并根据日志级别过滤和分发日志记录。
 * 支持动态添加/清除 Sink，使用快照机制保证无锁读取。
 * 同时满足 LogWriter 和 StructuredLogWriter 概念。
 */
class Logger {
public:
    /**
     * @brief 构造日志器
     * @param level 最低日志级别（默认为 kTrace，即记录所有级别）
     */
    explicit Logger(LogLevel level = LogLevel::kTrace) noexcept;

    /**
     * @brief Logger 持有互斥锁、原子快照指针和历史快照，禁止移动
     */
    Logger(Logger&&) = delete;

    /**
     * @brief Logger 持有互斥锁、原子快照指针和历史快照，禁止移动赋值
     */
    Logger& operator=(Logger&&) = delete;

    /**
     * @brief Sink 快照，用于无锁读取
     * @details 复制快照必须通过 clone() 显式完成，避免隐藏 shared_ptr 容器复制成本。
     */
    struct SinkSnapshot {
        SinkSnapshot() = default;
        SinkSnapshot(SinkSnapshot&&) noexcept = default;
        SinkSnapshot& operator=(SinkSnapshot&&) noexcept = default;

        /**
         * @brief 显式复制 Sink 指针容器
         * @return 拥有独立 vector 存储的新快照，Sink 对象仍保持共享所有权
         */
        [[nodiscard]] SinkSnapshot clone() const {
            SinkSnapshot copy;
            copy.sinks = sinks;
            return copy;
        }

        std::vector<std::shared_ptr<LogSink>> sinks; ///< Sink 列表

    private:
        SinkSnapshot(const SinkSnapshot&) = default;
        SinkSnapshot& operator=(const SinkSnapshot&) = default;
    };

    /**
     * @brief 设置最低日志级别
     * @param level 新的最低级别
     */
    void set_level(LogLevel level) noexcept;

    /**
     * @brief 获取当前最低日志级别
     * @return 当前级别
     */
    [[nodiscard]] LogLevel level() const noexcept;

    /**
     * @brief 检查给定级别是否启用
     * @param level 待检查的级别
     * @return 启用返回 true
     */
    [[nodiscard]] bool is_enabled(LogLevel level) const noexcept;

    /**
     * @brief 添加一个日志 Sink
     * @param sink LogSink 的共享指针
     */
    void add_sink(std::shared_ptr<LogSink> sink);

    /**
     * @brief 清除所有日志 Sink
     */
    void clear_sinks();

    /**
     * @brief 使用线程本地上下文记录日志
     * @tparam Args 格式化参数类型
     * @param level 日志级别
     * @param source 源码位置
     * @param fmt 格式化字符串
     * @param args 格式化参数
     */
    template <typename... Args>
    void log(LogLevel level, SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
        if (!is_enabled(level)) {
            return;
        }

        publish(LogRecord(
            level,
            std::format(fmt, std::forward<Args>(args)...),
            source,
            make_log_context(current_context())));
    }

    /**
     * @brief 使用显式上下文记录日志（协程安全入口）
     * @details 适用于无法依赖线程本地 current_context() 的协程任务
     * @tparam Args 格式化参数类型
     * @param level 日志级别
     * @param source 源码位置
     * @param context 显式追踪上下文
     * @param fmt 格式化字符串
     * @param args 格式化参数
     */
    template <typename... Args>
    void log_with_context(
        LogLevel level,
        SourceLocation source,
        std::optional<TraceContext> context,
        std::format_string<Args...> fmt,
        Args&&... args) {
        if (!is_enabled(level)) {
            return;
        }

        publish(LogRecord(
            level,
            std::format(fmt, std::forward<Args>(args)...),
            source,
            make_log_context(std::move(context))));
    }

    /**
     * @brief 写入普通日志记录（满足 LogWriter 概念）
     * @param record 日志记录
     */
    void write(LogRecord record);

    /**
     * @brief 写入结构化日志记录（满足 StructuredLogWriter 概念）
     * @param record 结构化日志记录
     */
    void write(StructuredLogRecord record);

private:
    /**
     * @brief 将日志记录发布到所有 Sink
     * @param record 日志记录
     */
    void publish(LogRecord record);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex m_mutex;                                         ///< Sink 管理互斥锁
    std::vector<std::unique_ptr<SinkSnapshot>> m_sinkSnapshots; ///< Sink 快照历史
    std::atomic<const SinkSnapshot*> m_sinkSnapshot{nullptr};   ///< 当前活跃的 Sink 快照
    std::atomic<LogLevel> m_level;                              ///< 原子日志级别
};

/**
 * @brief 获取进程级默认 Logger
 * @return 默认 Logger 的引用
 */
[[nodiscard]] Logger& default_logger() noexcept;

/**
 * @brief 设置进程级默认 Logger
 * @param logger Logger 指针（不获取所有权）
 */
void set_default_logger(Logger* logger) noexcept;

/**
 * @brief 清除默认日志写入器
 */
void set_default_log_writer(std::nullptr_t) noexcept;

/**
 * @brief 设置进程级默认写入器（不获取所有权）
 * @details 调用方须保证 writer 在默认写入器被更改或清除之前保持存活。
 * @tparam Writer 满足 LogWriter 或 StructuredLogWriter 概念的类型
 * @param writer 写入器指针，传入 nullptr 清除默认写入器
 */
template <typename Writer>
    requires(LogWriter<Writer> || StructuredLogWriter<Writer>)
void set_default_log_writer(Writer* writer) noexcept {
    if (writer == nullptr) {
        detail::set_default_log_writer_ref({});
        return;
    }

    detail::set_default_log_writer_ref(detail::ErasedLogWriter{
        .object = writer,
        .is_enabled_fn = [](const void* object, LogLevel level) noexcept {
            return static_cast<const Writer*>(object)->is_enabled(level);
        },
        .write_fn = detail::log_write_fn<Writer>(),
        .write_structured_fn = detail::structured_log_write_fn<Writer>(),
    });
}

/**
 * @brief 轻量级上下文绑定日志代理
 * @details 绑定追踪上下文和 Writer，提供 trace/debug/info/warn/error 级别的日志方法。
 * 显式 Writer 使用静态分发；使用默认 writer 引用时通过运行时分发。
 * @tparam Writer 满足 LogWriter 概念的写入器类型
 */
template <LogWriter Writer>
class ContextLogger {
public:
    /**
     * @brief 构造上下文日志代理
     * @param context 追踪上下文
     * @param writer 日志写入器
     * @param source 源码位置
     */
    ContextLogger(std::optional<TraceContext> context, Writer writer, SourceLocation source)
        : m_writer(std::move(writer)),
          m_source(source),
          m_context(make_log_context(std::move(context))) {
    }

    /**
     * @brief 记录追踪级别日志
     */
    template <typename... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) {
        write<LogLevel::kTrace>(fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 记录调试级别日志
     */
    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) {
        write<LogLevel::kDebug>(fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 记录信息级别日志
     */
    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) {
        write<LogLevel::kInfo>(fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 记录警告级别日志
     */
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) {
        write<LogLevel::kWarn>(fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 记录错误级别日志
     */
    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) {
        write<LogLevel::kError>(fmt, std::forward<Args>(args)...);
    }

private:
    /**
     * @brief 内部写入实现
     * @tparam kLevel 日志级别
     * @tparam Args 格式化参数类型
     */
    template <LogLevel kLevel, typename... Args>
    void write(std::format_string<Args...> fmt, Args&&... args) {
        if (!m_writer.is_enabled(kLevel)) {
            return;
        }

        m_writer.write(LogRecord(
            kLevel,
            std::format(fmt, std::forward<Args>(args)...),
            m_source,
            m_context));
    }

    Writer m_writer;                     ///< 日志写入器
    SourceLocation m_source;             ///< 源码位置
    std::optional<LogContext> m_context; ///< 绑定的日志上下文
};

/**
 * @brief 轻量级上下文绑定结构化事件代理
 * @details 字段仅在 write 调用期间借用；写入器不得保留字段 span。
 * @tparam Writer 满足 StructuredLogWriter 概念的写入器类型
 */
template <StructuredLogWriter Writer>
class ContextEventLogger {
public:
    /**
     * @brief 构造上下文事件日志代理
     * @param context 追踪上下文
     * @param writer 结构化日志写入器
     * @param source 源码位置
     */
    ContextEventLogger(std::optional<TraceContext> context, Writer writer, SourceLocation source)
        : m_writer(std::move(writer)),
          m_source(source),
          m_context(make_log_context(std::move(context))) {
    }

    /**
     * @brief 记录追踪级别结构化事件
     */
    template <typename... Fields>
    void trace(std::string_view name, Fields&&... fields) {
        write<LogLevel::kTrace>(name, std::forward<Fields>(fields)...);
    }

    /**
     * @brief 记录调试级别结构化事件
     */
    template <typename... Fields>
    void debug(std::string_view name, Fields&&... fields) {
        write<LogLevel::kDebug>(name, std::forward<Fields>(fields)...);
    }

    /**
     * @brief 记录信息级别结构化事件
     */
    template <typename... Fields>
    void info(std::string_view name, Fields&&... fields) {
        write<LogLevel::kInfo>(name, std::forward<Fields>(fields)...);
    }

    /**
     * @brief 记录警告级别结构化事件
     */
    template <typename... Fields>
    void warn(std::string_view name, Fields&&... fields) {
        write<LogLevel::kWarn>(name, std::forward<Fields>(fields)...);
    }

    /**
     * @brief 记录错误级别结构化事件
     */
    template <typename... Fields>
    void error(std::string_view name, Fields&&... fields) {
        write<LogLevel::kError>(name, std::forward<Fields>(fields)...);
    }

private:
    /**
     * @brief 内部结构化事件写入实现
     * @tparam kLevel 日志级别
     * @tparam Fields 字段类型列表
     */
    template <LogLevel kLevel, typename... Fields>
    void write(std::string_view name, Fields&&... fields) {
        if (!m_writer.is_enabled(kLevel)) {
            return;
        }

        std::array<LogField, sizeof...(Fields)> fieldArray{std::forward<Fields>(fields)...};
        m_writer.write(StructuredLogRecord{
            .level = kLevel,
            .name = name,
            .fields = std::span<const LogField>(fieldArray.data(), fieldArray.size()),
            .source = m_source,
            .context = m_context,
        });
    }

    Writer m_writer;                     ///< 结构化日志写入器
    SourceLocation m_source;             ///< 源码位置
    std::optional<LogContext> m_context; ///< 绑定的日志上下文
};

/**
 * @brief 创建绑定当前进程默认写入器的上下文日志代理
 * @param context 追踪上下文
 * @param source 源码位置（默认为调用点）
 * @return ContextLogger 实例
 */
[[nodiscard]] inline ContextLogger<detail::DefaultLogWriter> log(
    std::optional<TraceContext> context,
    SourceLocation source = SourceLocation::current()) {
    return ContextLogger<detail::DefaultLogWriter>(
        std::move(context),
        detail::default_log_writer(),
        source);
}

/**
 * @brief 创建绑定显式写入器的上下文日志代理
 * @tparam Writer 满足 LogWriter 概念的写入器类型
 * @param context 追踪上下文
 * @param writer 日志写入器引用
 * @param source 源码位置（默认为调用点）
 * @return ContextLogger 实例
 */
template <LogWriter Writer>
[[nodiscard]] ContextLogger<detail::BorrowedLogWriter<Writer>> log(
    std::optional<TraceContext> context,
    Writer& writer,
    SourceLocation source = SourceLocation::current()) {
    return ContextLogger<detail::BorrowedLogWriter<Writer>>(
        std::move(context),
        detail::BorrowedLogWriter<Writer>(writer),
        source);
}

/**
 * @brief 创建绑定当前默认写入器的上下文结构化事件代理
 * @param context 追踪上下文
 * @param source 源码位置（默认为调用点）
 * @return ContextEventLogger 实例
 */
[[nodiscard]] inline ContextEventLogger<detail::DefaultLogWriter> event(
    std::optional<TraceContext> context,
    SourceLocation source = SourceLocation::current()) {
    return ContextEventLogger<detail::DefaultLogWriter>(
        std::move(context),
        detail::default_log_writer(),
        source);
}

/**
 * @brief 创建绑定显式写入器的上下文结构化事件代理
 * @tparam Writer 满足 StructuredLogWriter 概念的写入器类型
 * @param context 追踪上下文
 * @param writer 结构化日志写入器引用
 * @param source 源码位置（默认为调用点）
 * @return ContextEventLogger 实例
 */
template <StructuredLogWriter Writer>
[[nodiscard]] ContextEventLogger<detail::BorrowedLogWriter<Writer>> event(
    std::optional<TraceContext> context,
    Writer& writer,
    SourceLocation source = SourceLocation::current()) {
    return ContextEventLogger<detail::BorrowedLogWriter<Writer>>(
        std::move(context),
        detail::BorrowedLogWriter<Writer>(writer),
        source);
}

/**
 * @brief 在指定级别使用默认写入器记录日志（使用线程本地上下文）
 * @tparam kLevel 日志级别
 * @tparam Args 格式化参数类型
 * @param source 源码位置
 * @param fmt 格式化字符串
 * @param args 格式化参数
 */
template <LogLevel kLevel, typename... Args>
void log_at(SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
    auto writer = detail::default_log_writer();
    if (!writer.is_enabled(kLevel)) {
        return;
    }

    writer.write(LogRecord(
        kLevel,
        std::format(fmt, std::forward<Args>(args)...),
        source,
        make_log_context(current_context())));
}

/**
 * @brief 在指定级别使用默认写入器记录日志（使用显式上下文）
 * @tparam kLevel 日志级别
 * @tparam Args 格式化参数类型
 * @param source 源码位置
 * @param context 显式追踪上下文
 * @param fmt 格式化字符串
 * @param args 格式化参数
 */
template <LogLevel kLevel, typename... Args>
void log_with_context_at(
    SourceLocation source,
    std::optional<TraceContext> context,
    std::format_string<Args...> fmt,
    Args&&... args) {
    auto writer = detail::default_log_writer();
    if (!writer.is_enabled(kLevel)) {
        return;
    }

    writer.write(LogRecord(
        kLevel,
        std::format(fmt, std::forward<Args>(args)...),
        source,
        make_log_context(std::move(context))));
}

/**
 * @brief 追踪级别日志（带源码位置）
 * @tparam Args 格式化参数类型
 * @param source 源码位置
 * @param fmt 格式化字符串
 * @param args 格式化参数
 */
template <typename... Args>
void log_trace_at(SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
    log_at<LogLevel::kTrace>(source, fmt, std::forward<Args>(args)...);
}

/**
 * @brief 追踪级别日志（带源码位置和显式上下文）
 */
template <typename... Args>
void log_trace_with_context_at(
    SourceLocation source,
    std::optional<TraceContext> context,
    std::format_string<Args...> fmt,
    Args&&... args) {
    log_with_context_at<LogLevel::kTrace>(source, std::move(context), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 调试级别日志（带源码位置）
 */
template <typename... Args>
void log_debug_at(SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
    log_at<LogLevel::kDebug>(source, fmt, std::forward<Args>(args)...);
}

/**
 * @brief 调试级别日志（带源码位置和显式上下文）
 */
template <typename... Args>
void log_debug_with_context_at(
    SourceLocation source,
    std::optional<TraceContext> context,
    std::format_string<Args...> fmt,
    Args&&... args) {
    log_with_context_at<LogLevel::kDebug>(source, std::move(context), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 信息级别日志（带源码位置）
 */
template <typename... Args>
void log_info_at(SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
    log_at<LogLevel::kInfo>(source, fmt, std::forward<Args>(args)...);
}

/**
 * @brief 信息级别日志（带源码位置和显式上下文）
 */
template <typename... Args>
void log_info_with_context_at(
    SourceLocation source,
    std::optional<TraceContext> context,
    std::format_string<Args...> fmt,
    Args&&... args) {
    log_with_context_at<LogLevel::kInfo>(source, std::move(context), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 警告级别日志（带源码位置）
 */
template <typename... Args>
void log_warn_at(SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
    log_at<LogLevel::kWarn>(source, fmt, std::forward<Args>(args)...);
}

/**
 * @brief 警告级别日志（带源码位置和显式上下文）
 */
template <typename... Args>
void log_warn_with_context_at(
    SourceLocation source,
    std::optional<TraceContext> context,
    std::format_string<Args...> fmt,
    Args&&... args) {
    log_with_context_at<LogLevel::kWarn>(source, std::move(context), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 错误级别日志（带源码位置）
 */
template <typename... Args>
void log_error_at(SourceLocation source, std::format_string<Args...> fmt, Args&&... args) {
    log_at<LogLevel::kError>(source, fmt, std::forward<Args>(args)...);
}

/**
 * @brief 错误级别日志（带源码位置和显式上下文）
 */
template <typename... Args>
void log_error_with_context_at(
    SourceLocation source,
    std::optional<TraceContext> context,
    std::format_string<Args...> fmt,
    Args&&... args) {
    log_with_context_at<LogLevel::kError>(source, std::move(context), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 追踪级别日志（自动捕获源码位置）
 */
template <typename... Args>
void log_trace(std::format_string<Args...> fmt, Args&&... args) {
    log_trace_at(SourceLocation::current(), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 调试级别日志（自动捕获源码位置）
 */
template <typename... Args>
void log_debug(std::format_string<Args...> fmt, Args&&... args) {
    log_debug_at(SourceLocation::current(), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 信息级别日志（自动捕获源码位置）
 */
template <typename... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
    log_info_at(SourceLocation::current(), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 警告级别日志（自动捕获源码位置）
 */
template <typename... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    log_warn_at(SourceLocation::current(), fmt, std::forward<Args>(args)...);
}

/**
 * @brief 错误级别日志（自动捕获源码位置）
 */
template <typename... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
    log_error_at(SourceLocation::current(), fmt, std::forward<Args>(args)...);
}

} // namespace galay::tracing

#define GALAY_LOG_TRACE(...) \
    ::galay::tracing::log_trace_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, __VA_ARGS__)
#define GALAY_LOG_TRACE_CTX(context, ...) \
    ::galay::tracing::log_trace_with_context_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, (context), __VA_ARGS__)
#define GALAY_LOG_DEBUG(...) \
    ::galay::tracing::log_debug_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, __VA_ARGS__)
#define GALAY_LOG_DEBUG_CTX(context, ...) \
    ::galay::tracing::log_debug_with_context_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, (context), __VA_ARGS__)
#define GALAY_LOG_INFO(...) \
    ::galay::tracing::log_info_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, __VA_ARGS__)
#define GALAY_LOG_INFO_CTX(context, ...) \
    ::galay::tracing::log_info_with_context_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, (context), __VA_ARGS__)
#define GALAY_LOG_WARN(...) \
    ::galay::tracing::log_warn_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, __VA_ARGS__)
#define GALAY_LOG_WARN_CTX(context, ...) \
    ::galay::tracing::log_warn_with_context_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, (context), __VA_ARGS__)
#define GALAY_LOG_ERROR(...) \
    ::galay::tracing::log_error_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, __VA_ARGS__)
#define GALAY_LOG_ERROR_CTX(context, ...) \
    ::galay::tracing::log_error_with_context_at(::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__}, (context), __VA_ARGS__)

// Rust-like structured event callsite. The level check happens before context
// and field expressions are evaluated, so disabled events avoid field creation.
#define GALAY_EVENT_AT(level, writer, context, name, ...)                                                   \
    do {                                                                                                    \
        auto&& galayTracingWriter = (writer);                                                               \
        constexpr auto galayTracingLevel = (level);                                                         \
        if (galayTracingWriter.is_enabled(galayTracingLevel)) {                                              \
            ::galay::tracing::detail::write_event_unchecked<galayTracingLevel>(                               \
                galayTracingWriter,                                                                         \
                (context),                                                                                  \
                ::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__},                             \
                (name) __VA_OPT__(, ) __VA_ARGS__);                                                         \
        }                                                                                                   \
    } while (false)

#define GALAY_EVENT_DEFAULT_AT(level, context, name, ...)                                                   \
    do {                                                                                                    \
        const auto* galayTracingWriter = ::galay::tracing::detail::default_log_writer_ptr();                   \
        constexpr auto galayTracingLevel = (level);                                                         \
        if (galayTracingWriter != nullptr && galayTracingWriter->is_enabled(galayTracingLevel)) {            \
            ::galay::tracing::detail::write_event_unchecked<galayTracingLevel>(                               \
                *galayTracingWriter,                                                                        \
                (context),                                                                                  \
                ::galay::tracing::SourceLocation{__FILE__, __LINE__, __func__},                             \
                (name) __VA_OPT__(, ) __VA_ARGS__);                                                         \
        }                                                                                                   \
    } while (false)

#define GALAY_EVENT_TRACE(writer, context, name, ...) \
    GALAY_EVENT_AT(::galay::tracing::LogLevel::kTrace, writer, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_DEBUG(writer, context, name, ...) \
    GALAY_EVENT_AT(::galay::tracing::LogLevel::kDebug, writer, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_INFO(writer, context, name, ...) \
    GALAY_EVENT_AT(::galay::tracing::LogLevel::kInfo, writer, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_WARN(writer, context, name, ...) \
    GALAY_EVENT_AT(::galay::tracing::LogLevel::kWarn, writer, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_ERROR(writer, context, name, ...) \
    GALAY_EVENT_AT(::galay::tracing::LogLevel::kError, writer, context, name __VA_OPT__(, ) __VA_ARGS__)

#define GALAY_EVENT_TRACE_DEFAULT(context, name, ...) \
    GALAY_EVENT_DEFAULT_AT(::galay::tracing::LogLevel::kTrace, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_DEBUG_DEFAULT(context, name, ...) \
    GALAY_EVENT_DEFAULT_AT(::galay::tracing::LogLevel::kDebug, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_INFO_DEFAULT(context, name, ...) \
    GALAY_EVENT_DEFAULT_AT(::galay::tracing::LogLevel::kInfo, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_WARN_DEFAULT(context, name, ...) \
    GALAY_EVENT_DEFAULT_AT(::galay::tracing::LogLevel::kWarn, context, name __VA_OPT__(, ) __VA_ARGS__)
#define GALAY_EVENT_ERROR_DEFAULT(context, name, ...) \
    GALAY_EVENT_DEFAULT_AT(::galay::tracing::LogLevel::kError, context, name __VA_OPT__(, ) __VA_ARGS__)
