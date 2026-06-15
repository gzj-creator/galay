/**
 * @file log_record.h
 * @brief 日志记录与结构化事件数据类型定义
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义日志系统的核心数据类型，包括：
 * - LogFieldValue：非拥有的结构化字段值（支持 int64/uint64/double/bool/string_view）
 * - LogField：结构化字段键值对
 * - LogRecord：标准日志记录
 * - StructuredLogRecord：零分配的结构化事件记录
 * 以及字段创建工具函数。
 */

#pragma once

#include "galay-tracing/common/source_location.h"
#include "galay-tracing/context/trace_context.h"
#include "galay-tracing/log/log_level.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace galay::tracing {

/**
 * @brief 日志字段值类型枚举
 */
enum class LogFieldType {
    kInt64,   ///< 64 位有符号整数
    kUInt64,  ///< 64 位无符号整数
    kDouble,  ///< 双精度浮点数
    kBool,    ///< 布尔值
    kString,  ///< 字符串视图
};

/**
 * @brief 非拥有的结构化日志字段值
 * @details 用于 event(...) 调用的结构化字段值。字符串值（string_view）必须
 * 在包含此值的 StructuredLogRecord 的 write() 调用返回之前保持有效。
 * 使用联合体实现零分配的值存储。
 */
class LogFieldValue {
public:
    constexpr LogFieldValue() noexcept = default;

    /**
     * @brief 从 int64 创建字段值
     * @param value 整数值
     * @return LogFieldValue 实例
     */
    [[nodiscard]] static constexpr LogFieldValue fromInt64(std::int64_t value) noexcept {
        return LogFieldValue(LogFieldType::kInt64, Storage(value));
    }

    /**
     * @brief 从 uint64 创建字段值
     * @param value 无符号整数值
     * @return LogFieldValue 实例
     */
    [[nodiscard]] static constexpr LogFieldValue fromUInt64(std::uint64_t value) noexcept {
        return LogFieldValue(LogFieldType::kUInt64, Storage(value));
    }

    /**
     * @brief 从 double 创建字段值
     * @param value 浮点数值
     * @return LogFieldValue 实例
     */
    [[nodiscard]] static constexpr LogFieldValue fromDouble(double value) noexcept {
        return LogFieldValue(LogFieldType::kDouble, Storage(value));
    }

    /**
     * @brief 从 bool 创建字段值
     * @param value 布尔值
     * @return LogFieldValue 实例
     */
    [[nodiscard]] static constexpr LogFieldValue fromBool(bool value) noexcept {
        return LogFieldValue(LogFieldType::kBool, Storage(value));
    }

    /**
     * @brief 从 string_view 创建字段值
     * @param value 字符串视图（须保持有效直到 write 返回）
     * @return LogFieldValue 实例
     */
    [[nodiscard]] static constexpr LogFieldValue fromString(std::string_view value) noexcept {
        return LogFieldValue(LogFieldType::kString, Storage(value));
    }

    /**
     * @brief 获取值类型
     * @return LogFieldType 枚举值
     */
    [[nodiscard]] constexpr LogFieldType type() const noexcept {
        return m_type;
    }

    /**
     * @brief 以 int64 获取值
     */
    [[nodiscard]] constexpr std::int64_t asInt64() const noexcept {
        return m_storage.int64Value;
    }

    /**
     * @brief 以 uint64 获取值
     */
    [[nodiscard]] constexpr std::uint64_t asUInt64() const noexcept {
        return m_storage.uint64Value;
    }

    /**
     * @brief 以 double 获取值
     */
    [[nodiscard]] constexpr double asDouble() const noexcept {
        return m_storage.doubleValue;
    }

    /**
     * @brief 以 bool 获取值
     */
    [[nodiscard]] constexpr bool asBool() const noexcept {
        return m_storage.boolValue;
    }

    /**
     * @brief 以 string_view 获取值
     */
    [[nodiscard]] constexpr std::string_view asString() const noexcept {
        return m_storage.stringValue;
    }

private:
    /**
     * @brief 值存储联合体
     */
    union Storage {
        constexpr Storage() noexcept
            : stringValue() {
        }

        explicit constexpr Storage(std::int64_t value) noexcept
            : int64Value(value) {
        }

        explicit constexpr Storage(std::uint64_t value) noexcept
            : uint64Value(value) {
        }

        explicit constexpr Storage(double value) noexcept
            : doubleValue(value) {
        }

        explicit constexpr Storage(bool value) noexcept
            : boolValue(value) {
        }

        explicit constexpr Storage(std::string_view value) noexcept
            : stringValue(value) {
        }

        std::int64_t int64Value;       ///< int64 值
        std::uint64_t uint64Value;     ///< uint64 值
        double doubleValue;            ///< double 值
        bool boolValue;                ///< bool 值
        std::string_view stringValue;  ///< string_view 值
    };

    explicit constexpr LogFieldValue(LogFieldType type, Storage storage) noexcept
        : m_type(type),
          m_storage(storage) {
    }

    LogFieldType m_type{LogFieldType::kString}; ///< 值类型
    Storage m_storage{};                         ///< 值存储
};

/**
 * @brief 结构化日志字段键值对
 */
struct LogField {
    std::string_view name;   ///< 字段名称
    LogFieldValue value;     ///< 字段值
};

/**
 * @brief 创建 int64 类型的日志字段
 * @param name 字段名称
 * @param value 字段值
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, std::int64_t value) noexcept {
    return LogField{.name = name, .value = LogFieldValue::fromInt64(value)};
}

/**
 * @brief 创建 int 类型的日志字段
 * @param name 字段名称
 * @param value 字段值
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, int value) noexcept {
    return field(name, static_cast<std::int64_t>(value));
}

/**
 * @brief 创建 uint64 类型的日志字段
 * @param name 字段名称
 * @param value 字段值
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, std::uint64_t value) noexcept {
    return LogField{.name = name, .value = LogFieldValue::fromUInt64(value)};
}

/**
 * @brief 创建 double 类型的日志字段
 * @param name 字段名称
 * @param value 字段值
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, double value) noexcept {
    return LogField{.name = name, .value = LogFieldValue::fromDouble(value)};
}

/**
 * @brief 创建 bool 类型的日志字段
 * @param name 字段名称
 * @param value 字段值
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, bool value) noexcept {
    return LogField{.name = name, .value = LogFieldValue::fromBool(value)};
}

/**
 * @brief 创建 string_view 类型的日志字段
 * @param name 字段名称
 * @param value 字段值
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, std::string_view value) noexcept {
    return LogField{.name = name, .value = LogFieldValue::fromString(value)};
}

/**
 * @brief 创建 C 字符串类型的日志字段
 * @param name 字段名称
 * @param value 字段值（nullptr 安全处理为空字符串）
 * @return LogField 键值对
 */
[[nodiscard]] constexpr LogField field(std::string_view name, const char* value) noexcept {
    return field(name, std::string_view(value == nullptr ? "" : value));
}

/**
 * @brief 标准日志记录
 * @details 包含日志级别、消息、源码位置、追踪上下文和时间戳的完整日志记录。
 */
struct LogRecord {
    LogLevel level{LogLevel::kInfo};                                     ///< 日志级别
    std::string message;                                                 ///< 日志消息
    SourceLocation source;                                               ///< 源码位置
    std::optional<LogContext> context;                                   ///< 追踪上下文
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()}; ///< 时间戳
};

/**
 * @brief 零分配的结构化事件记录
 * @details 字段存储从日志调用点借用，仅在 writer 的 write() 调用期间有效。
 * writer 不得保留字段 span 的引用。
 */
struct StructuredLogRecord {
    LogLevel level{LogLevel::kInfo};         ///< 日志级别
    std::string_view name;                   ///< 事件名称
    std::span<const LogField> fields;        ///< 结构化字段视图
    SourceLocation source;                   ///< 源码位置
    std::optional<LogContext> context;       ///< 追踪上下文
};

} // namespace galay::tracing
