/**
 * @file span.h
 * @brief Span 核心数据类型与属性定义
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义分布式追踪中 Span 的完整数据模型，包括 Span 属性值、
 * Span 状态、Span 类型（SpanKind）、时间策略，以及 Span 类本身。
 * Span 是追踪中的基本工作单元，记录一次操作的名称、上下文、属性和状态。
 */

#pragma once

#include "galay-tracing/context/trace_context.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace galay::tracing {

/**
 * @brief Span 时间记录策略
 */
enum class SpanTimingPolicy {
    kDisabled,  ///< 禁用时间戳记录（默认）
    kEnabled,   ///< 启用开始/结束时间戳记录
};

/**
 * @brief Span 类型
 */
enum class SpanKind {
    kInternal,  ///< 内部 Span（不跨越服务边界）
    kServer,    ///< 服务端 Span
    kClient,    ///< 客户端 Span
    kProducer,  ///< 消息生产者 Span
    kConsumer,  ///< 消息消费者 Span
};

/**
 * @brief Span 状态码
 */
enum class SpanStatusCode {
    kUnset,  ///< 未设置状态
    kOk,     ///< 操作成功
    kError,  ///< 操作出错
};

/**
 * @brief Span 属性值类型枚举
 */
enum class SpanAttributeType {
    kInt64,   ///< 64 位有符号整数
    kUInt64,  ///< 64 位无符号整数
    kDouble,  ///< 双精度浮点数
    kBool,    ///< 布尔值
    kString,  ///< 字符串
};

/**
 * @brief 拥有所有权的 Span 属性值
 * @details Span 属性可能被异步导出，因此字符串值从调用点复制而非借用。
 * 支持五种数据类型：int64、uint64、double、bool 和 string。
 */
class SpanAttributeValue {
public:
    SpanAttributeValue() = default;

    /**
     * @brief 从 int64 值创建属性值
     * @param value 整数值
     * @return 对应的 SpanAttributeValue
     */
    [[nodiscard]] static SpanAttributeValue fromInt64(std::int64_t value);

    /**
     * @brief 从 uint64 值创建属性值
     * @param value 无符号整数值
     * @return 对应的 SpanAttributeValue
     */
    [[nodiscard]] static SpanAttributeValue fromUInt64(std::uint64_t value);

    /**
     * @brief 从 double 值创建属性值
     * @param value 浮点数值
     * @return 对应的 SpanAttributeValue
     */
    [[nodiscard]] static SpanAttributeValue fromDouble(double value);

    /**
     * @brief 从 bool 值创建属性值
     * @param value 布尔值
     * @return 对应的 SpanAttributeValue
     */
    [[nodiscard]] static SpanAttributeValue fromBool(bool value);

    /**
     * @brief 从 string 值创建属性值
     * @param value 字符串值（会被移动）
     * @return 对应的 SpanAttributeValue
     */
    [[nodiscard]] static SpanAttributeValue fromString(std::string value);

    /**
     * @brief 获取属性值类型
     * @return 当前存储的数据类型
     */
    [[nodiscard]] SpanAttributeType type() const noexcept;

    /**
     * @brief 以 int64 类型获取值
     * @return int64 值
     */
    [[nodiscard]] std::int64_t asInt64() const;

    /**
     * @brief 以 uint64 类型获取值
     * @return uint64 值
     */
    [[nodiscard]] std::uint64_t asUInt64() const;

    /**
     * @brief 以 double 类型获取值
     * @return double 值
     */
    [[nodiscard]] double asDouble() const;

    /**
     * @brief 以 bool 类型获取值
     * @return bool 值
     */
    [[nodiscard]] bool asBool() const;

    /**
     * @brief 以 string 类型获取值
     * @return 字符串的常量引用
     */
    [[nodiscard]] const std::string& asString() const;

private:
    using Storage = std::variant<std::int64_t, std::uint64_t, double, bool, std::string>;

    explicit SpanAttributeValue(Storage storage);

    Storage m_storage{std::string()}; ///< 类型安全的值存储
};

/**
 * @brief Span 属性键值对
 */
struct SpanAttribute {
    std::string name;           ///< 属性名称
    SpanAttributeValue value;   ///< 属性值
};

/**
 * @brief 创建 int64 类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, std::int64_t value);

/**
 * @brief 创建 int 类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, int value);

/**
 * @brief 创建 uint64 类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, std::uint64_t value);

/**
 * @brief 创建 double 类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, double value);

/**
 * @brief 创建 bool 类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, bool value);

/**
 * @brief 创建 string_view 类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, std::string_view value);

/**
 * @brief 创建 C 字符串类型的 Span 属性
 * @param name 属性名称
 * @param value 属性值
 * @return SpanAttribute 键值对
 */
[[nodiscard]] SpanAttribute spanAttribute(std::string_view name, const char* value);

/**
 * @brief Span 状态
 */
struct SpanStatus {
    SpanStatusCode code{SpanStatusCode::kUnset}; ///< 状态码
    std::string message;                         ///< 状态描述消息
};

/**
 * @brief 设置 Span 时间戳记录策略
 * @details 控制是否记录 Span 的开始和结束时间戳。默认禁用，
 * 以日志时间戳作为低成本的默认时间来源。
 * @param policy 时间策略
 */
void setSpanTimingPolicy(SpanTimingPolicy policy) noexcept;

/**
 * @brief 获取当前 Span 时间戳记录策略
 * @return 当前策略
 */
[[nodiscard]] SpanTimingPolicy spanTimingPolicy() noexcept;

/**
 * @brief 追踪 Span 数据模型
 * @details 表示分布式追踪中的一个工作单元，包含操作名称、追踪上下文、
 * 属性列表、状态和时间信息。Span 在结束时被提交给 SpanProcessor 处理。
 */
class Span {
public:
    using Clock = std::chrono::steady_clock; ///< 时钟类型
    static constexpr std::size_t kMaxAttributes = 32; ///< 单个 Span 最大属性数量

    Span() = default;

    /**
     * @brief 构造 Span（使用 TraceContext）
     * @param name 操作名称
     * @param context 追踪上下文
     */
    Span(std::string name, TraceContext context);

    /**
     * @brief 构造 Span（使用 TraceContext 和指定时间策略）
     * @param name 操作名称
     * @param context 追踪上下文
     * @param timingPolicy 时间记录策略
     */
    Span(std::string name, TraceContext context, SpanTimingPolicy timingPolicy);

    /**
     * @brief 构造 Span（使用 SpanContext）
     * @param name 操作名称
     * @param context Span 上下文
     * @param tracestate W3C tracestate 字符串
     */
    Span(std::string name, SpanContext context, std::string tracestate = {});

    /**
     * @brief 构造 Span（使用 SpanContext 和指定时间策略）
     * @param name 操作名称
     * @param context Span 上下文
     * @param tracestate W3C tracestate 字符串
     * @param timingPolicy 时间记录策略
     */
    Span(std::string name, SpanContext context, std::string tracestate, SpanTimingPolicy timingPolicy);

    /**
     * @brief 获取操作名称
     * @return 操作名称的字符串视图
     */
    [[nodiscard]] std::string_view name() const noexcept {
        return m_name;
    }

    /**
     * @brief 获取完整的追踪上下文（包含 tracestate）
     * @return TraceContext 实例
     */
    [[nodiscard]] TraceContext context() const {
        return m_context.toTraceContext(m_tracestate);
    }

    /**
     * @brief 获取 Span 上下文
     * @return SpanContext 的常量引用
     */
    [[nodiscard]] const SpanContext& spanContext() const noexcept {
        return m_context;
    }

    /**
     * @brief 获取 W3C tracestate 字符串
     * @return tracestate 的常量引用
     */
    [[nodiscard]] const std::string& tracestate() const noexcept {
        return m_tracestate;
    }

    /**
     * @brief 获取 Span 类型
     * @return SpanKind 枚举值
     */
    [[nodiscard]] SpanKind kind() const noexcept {
        return m_kind;
    }

    /**
     * @brief 设置 Span 类型
     * @param kind Span 类型
     */
    void setKind(SpanKind kind) noexcept {
        m_kind = kind;
    }

    /**
     * @brief 获取 Span 状态
     * @return SpanStatus 的常量引用
     */
    [[nodiscard]] const SpanStatus& status() const noexcept {
        return m_status;
    }

    /**
     * @brief 设置 Span 状态
     * @param code 状态码
     * @param message 状态描述消息（默认为空）
     */
    void setStatus(SpanStatusCode code, std::string message = {});

    /**
     * @brief 获取所有属性
     * @return 属性列表的只读视图
     */
    [[nodiscard]] std::span<const SpanAttribute> attributes() const noexcept {
        return std::span<const SpanAttribute>(m_attributes.data(), m_attributes.size());
    }

    /**
     * @brief 设置属性（键值对形式）
     * @param attribute 属性键值对
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(SpanAttribute attribute);

    /**
     * @brief 设置 int64 属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, std::int64_t value);

    /**
     * @brief 设置 int 属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, int value);

    /**
     * @brief 设置 uint64 属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, std::uint64_t value);

    /**
     * @brief 设置 double 属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, double value);

    /**
     * @brief 设置 bool 属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, bool value);

    /**
     * @brief 设置 string_view 属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, std::string_view value);

    /**
     * @brief 设置 C 字符串属性
     * @param name 属性名称
     * @param value 属性值
     * @return 成功添加返回 true，已达上限返回 false
     */
    [[nodiscard]] bool setAttribute(std::string_view name, const char* value);

    /**
     * @brief 获取 Span 开始时间
     * @return 开始时间点
     */
    [[nodiscard]] Clock::time_point startedAt() const noexcept {
        return m_startedAt;
    }

    /**
     * @brief 获取 Span 结束时间
     * @return 结束时间点
     */
    [[nodiscard]] Clock::time_point endedAt() const noexcept {
        return m_endedAt;
    }

    /**
     * @brief 检查 Span 是否已结束
     * @return 已结束返回 true
     */
    [[nodiscard]] bool ended() const noexcept {
        return m_ended;
    }

    /**
     * @brief 标记 Span 为已结束
     */
    void end() noexcept;

private:
    std::string m_name;                        ///< 操作名称
    SpanContext m_context;                      ///< Span 上下文
    std::string m_tracestate;                   ///< W3C tracestate
    SpanKind m_kind{SpanKind::kInternal};       ///< Span 类型
    SpanStatus m_status;                        ///< Span 状态
    std::vector<SpanAttribute> m_attributes;    ///< 属性列表
    Clock::time_point m_startedAt{};            ///< 开始时间戳
    Clock::time_point m_endedAt{};              ///< 结束时间戳
    bool m_ended{false};                        ///< 是否已结束
};

} // namespace galay::tracing
