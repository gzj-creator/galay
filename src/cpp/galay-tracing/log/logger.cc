/**
 * @file logger.cc
 * @brief 日志系统核心：Logger、Writer、全局 API 实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 Logger 的 Sink 快照管理（无锁读取 + 写时复制）、
 * 类型擦除写入器的全局配置、结构化事件到普通日志的降级转换，
 * 以及进程级默认 Logger 和默认写入器的管理。
 */

#include "logger.h"

#include "console_sink.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace galay::tracing {

namespace {

std::atomic<Logger*> g_defaultLogger{nullptr};

struct DefaultLogWriterSnapshot {
    detail::ErasedLogWriter writer;
};

std::mutex g_defaultWriterConfigMutex;
std::vector<std::unique_ptr<DefaultLogWriterSnapshot>> g_defaultWriterSnapshots;

[[nodiscard]] detail::ErasedLogWriter logger_writer_ref(Logger& logger) noexcept {
    return detail::ErasedLogWriter{
        .object = &logger,
        .is_enabled_fn = [](const void* object, LogLevel level) noexcept {
            return static_cast<const Logger*>(object)->is_enabled(level);
        },
        .write_fn = [](void* object, LogRecord record) {
            static_cast<Logger*>(object)->write(std::move(record));
        },
        .write_structured_fn = [](void* object, StructuredLogRecord record) {
            static_cast<Logger*>(object)->write(record);
        },
    };
}

[[nodiscard]] Logger& built_in_default_logger() {
    static Logger logger;
    static const bool configured = [] {
        logger.add_sink(std::make_shared<ConsoleSink>());
        return true;
    }();
    (void)configured;
    return logger;
}

[[nodiscard]] const DefaultLogWriterSnapshot& built_in_default_writer_snapshot() {
    static const DefaultLogWriterSnapshot snapshot{logger_writer_ref(built_in_default_logger())};
    return snapshot;
}

void append_field_value(std::string& message, const LogFieldValue& value) {
    std::array<char, 32> buffer{};
    switch (value.type()) {
    case LogFieldType::kInt64: {
        auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.as_int64());
        if (error == std::errc{}) {
            message.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
        }
        break;
    }
    case LogFieldType::kUInt64: {
        auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.as_uint64());
        if (error == std::errc{}) {
            message.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
        }
        break;
    }
    case LogFieldType::kDouble:
        message.append(std::to_string(value.as_double()));
        break;
    case LogFieldType::kBool:
        message.append(value.as_bool() ? "true" : "false");
        break;
    case LogFieldType::kString:
        message.append(value.as_string());
        break;
    }
}

[[nodiscard]] LogRecord make_log_record(StructuredLogRecord record) {
    std::size_t estimatedSize = record.name.size();
    for (const auto& field : record.fields) {
        estimatedSize += field.name.size() + 2;
        if (field.value.type() == LogFieldType::kString) {
            estimatedSize += field.value.as_string().size();
        } else {
            estimatedSize += 24;
        }
    }
    std::string message;
    message.reserve(estimatedSize);
    message.append(record.name);
    for (const auto& field : record.fields) {
        message.push_back(' ');
        message.append(field.name);
        message.push_back('=');
        append_field_value(message, field.value);
    }

    return LogRecord(
        record.level,
        std::move(message),
        record.source,
        std::move(record.context));
}

} // namespace

namespace detail {

std::atomic<const ErasedLogWriter*> g_defaultLogWriterPtr{nullptr};

void ErasedLogWriter::write_structured_fallback(StructuredLogRecord record) const {
    if (write_fn != nullptr) {
        write_fn(object, make_log_record(std::move(record)));
    }
}

const ErasedLogWriter* built_in_default_log_writer_ptr() noexcept {
    return &built_in_default_writer_snapshot().writer;
}

void set_default_log_writer_ref(ErasedLogWriter writer) noexcept {
    if (writer.object == nullptr) {
        g_defaultLogWriterPtr.store(nullptr, std::memory_order_release);
        return;
    }

    auto next = std::make_unique<DefaultLogWriterSnapshot>(writer);
    auto* snapshot = next.get();
    {
        std::lock_guard lock(g_defaultWriterConfigMutex);
        // Keep old snapshots alive for lock-free readers that already loaded
        // the previous pointer. Default writer configuration is rare.
        g_defaultWriterSnapshots.push_back(std::move(next));
    }
    g_defaultLogWriterPtr.store(&snapshot->writer, std::memory_order_release);
}

ErasedLogWriter default_log_writer_ref() noexcept {
    return *default_log_writer_ptr();
}

DefaultLogWriter default_log_writer() noexcept {
    return DefaultLogWriter(default_log_writer_ptr());
}

} // namespace detail

Logger::Logger(LogLevel level) noexcept
    : m_level(level) {
    auto snapshot = std::make_unique<SinkSnapshot>();
    auto* snapshotPtr = snapshot.get();
    m_sinkSnapshots.push_back(std::move(snapshot));
    m_sinkSnapshot.store(snapshotPtr, std::memory_order_release);
}

void Logger::set_level(LogLevel level) noexcept {
    m_level.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return m_level.load(std::memory_order_relaxed);
}

bool Logger::is_enabled(LogLevel recordLevel) const noexcept {
    const auto threshold = level();
    return threshold != LogLevel::kOff && static_cast<int>(recordLevel) >= static_cast<int>(threshold);
}

void Logger::add_sink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto* current = m_sinkSnapshot.load(std::memory_order_acquire);
    auto next = current != nullptr
        ? std::make_unique<SinkSnapshot>(current->clone())
        : std::make_unique<SinkSnapshot>();
    next->sinks.push_back(std::move(sink));
    auto* snapshotPtr = next.get();
    m_sinkSnapshots.push_back(std::move(next));
    m_sinkSnapshot.store(snapshotPtr, std::memory_order_release);
}

void Logger::clear_sinks() {
    std::lock_guard lock(m_mutex);
    auto next = std::make_unique<SinkSnapshot>();
    auto* snapshotPtr = next.get();
    // Old snapshots stay owned by the logger so lock-free readers that already
    // loaded a previous pointer can finish without taking the configuration lock.
    m_sinkSnapshots.push_back(std::move(next));
    m_sinkSnapshot.store(snapshotPtr, std::memory_order_release);
}

void Logger::write(LogRecord record) {
    publish(std::move(record));
}

void Logger::write(StructuredLogRecord record) {
    publish(make_log_record(std::move(record)));
}

void Logger::publish(LogRecord record) {
    const auto* snapshot = m_sinkSnapshot.load(std::memory_order_acquire);
    if (snapshot == nullptr) {
        return;
    }

    for (const auto& sink : snapshot->sinks) {
        sink->write(record);
    }
}

Logger& default_logger() noexcept {
    if (auto* logger = g_defaultLogger.load(std::memory_order_acquire); logger != nullptr) {
        return *logger;
    }
    return built_in_default_logger();
}

void set_default_logger(Logger* logger) noexcept {
    g_defaultLogger.store(logger, std::memory_order_release);
    set_default_log_writer(logger);
}

void set_default_log_writer(std::nullptr_t) noexcept {
    detail::set_default_log_writer_ref({});
}

} // namespace galay::tracing
