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

#include "galay-tracing/log/logger.h"

#include "galay-tracing/log/console_sink.h"

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

[[nodiscard]] detail::ErasedLogWriter loggerWriterRef(Logger& logger) noexcept {
    return detail::ErasedLogWriter{
        .object = &logger,
        .isEnabledFn = [](const void* object, LogLevel level) noexcept {
            return static_cast<const Logger*>(object)->isEnabled(level);
        },
        .writeFn = [](void* object, LogRecord record) {
            static_cast<Logger*>(object)->write(std::move(record));
        },
        .writeStructuredFn = [](void* object, StructuredLogRecord record) {
            static_cast<Logger*>(object)->write(record);
        },
    };
}

[[nodiscard]] Logger& builtInDefaultLogger() {
    static Logger logger;
    static const bool configured = [] {
        logger.addSink(std::make_shared<ConsoleSink>());
        return true;
    }();
    (void)configured;
    return logger;
}

[[nodiscard]] const DefaultLogWriterSnapshot& builtInDefaultWriterSnapshot() {
    static const DefaultLogWriterSnapshot snapshot{loggerWriterRef(builtInDefaultLogger())};
    return snapshot;
}

void appendFieldValue(std::string& message, const LogFieldValue& value) {
    std::array<char, 32> buffer{};
    switch (value.type()) {
    case LogFieldType::kInt64: {
        auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.asInt64());
        if (error == std::errc{}) {
            message.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
        }
        break;
    }
    case LogFieldType::kUInt64: {
        auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.asUInt64());
        if (error == std::errc{}) {
            message.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
        }
        break;
    }
    case LogFieldType::kDouble:
        message.append(std::to_string(value.asDouble()));
        break;
    case LogFieldType::kBool:
        message.append(value.asBool() ? "true" : "false");
        break;
    case LogFieldType::kString:
        message.append(value.asString());
        break;
    }
}

[[nodiscard]] LogRecord makeLogRecord(StructuredLogRecord record) {
    std::size_t estimatedSize = record.name.size();
    for (const auto& field : record.fields) {
        estimatedSize += field.name.size() + 2;
        if (field.value.type() == LogFieldType::kString) {
            estimatedSize += field.value.asString().size();
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
        appendFieldValue(message, field.value);
    }

    return LogRecord{
        .level = record.level,
        .message = std::move(message),
        .source = record.source,
        .context = std::move(record.context),
    };
}

} // namespace

namespace detail {

std::atomic<const ErasedLogWriter*> g_defaultLogWriterPtr{nullptr};

void ErasedLogWriter::writeStructuredFallback(StructuredLogRecord record) const {
    if (writeFn != nullptr) {
        writeFn(object, makeLogRecord(record));
    }
}

const ErasedLogWriter* builtInDefaultLogWriterPtr() noexcept {
    return &builtInDefaultWriterSnapshot().writer;
}

void setDefaultLogWriterRef(ErasedLogWriter writer) noexcept {
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

ErasedLogWriter defaultLogWriterRef() noexcept {
    return *defaultLogWriterPtr();
}

DefaultLogWriter defaultLogWriter() noexcept {
    return DefaultLogWriter(defaultLogWriterPtr());
}

} // namespace detail

Logger::Logger(LogLevel level) noexcept
    : m_level(level) {
    auto snapshot = std::make_unique<SinkSnapshot>();
    auto* snapshotPtr = snapshot.get();
    m_sinkSnapshots.push_back(std::move(snapshot));
    m_sinkSnapshot.store(snapshotPtr, std::memory_order_release);
}

void Logger::setLevel(LogLevel level) noexcept {
    m_level.store(level, std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return m_level.load(std::memory_order_relaxed);
}

bool Logger::isEnabled(LogLevel recordLevel) const noexcept {
    const auto threshold = level();
    return threshold != LogLevel::kOff && static_cast<int>(recordLevel) >= static_cast<int>(threshold);
}

void Logger::addSink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }

    std::lock_guard lock(m_mutex);
    auto next = std::make_unique<SinkSnapshot>();
    if (auto* current = m_sinkSnapshot.load(std::memory_order_acquire); current != nullptr) {
        next->sinks = current->sinks;
    }
    next->sinks.push_back(std::move(sink));
    auto* snapshotPtr = next.get();
    m_sinkSnapshots.push_back(std::move(next));
    m_sinkSnapshot.store(snapshotPtr, std::memory_order_release);
}

void Logger::clearSinks() {
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
    publish(makeLogRecord(record));
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

Logger& defaultLogger() noexcept {
    if (auto* logger = g_defaultLogger.load(std::memory_order_acquire); logger != nullptr) {
        return *logger;
    }
    return builtInDefaultLogger();
}

void setDefaultLogger(Logger* logger) noexcept {
    g_defaultLogger.store(logger, std::memory_order_release);
    setDefaultLogWriter(logger);
}

void setDefaultLogWriter(std::nullptr_t) noexcept {
    detail::setDefaultLogWriterRef({});
}

} // namespace galay::tracing
