/**
 * @file span.cc
 * @brief Span 核心数据类型与属性操作实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 SpanAttributeValue 的类型安全存取、SpanAttribute 工厂函数、
 * 全局 Span 时间策略管理，以及 Span 的构造、属性设置和生命周期控制。
 */

#include "galay-tracing/kernel/span.h"

#include <atomic>
#include <string>
#include <utility>
#include <variant>

namespace galay::tracing {

namespace {

std::atomic<SpanTimingPolicy> g_spanTimingPolicy{SpanTimingPolicy::kDisabled};

} // namespace

SpanAttributeValue::SpanAttributeValue(Storage storage)
    : m_storage(std::move(storage)) {
}

SpanAttributeValue SpanAttributeValue::fromInt64(std::int64_t value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::fromUInt64(std::uint64_t value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::fromDouble(double value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::fromBool(bool value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::fromString(std::string value) {
    return SpanAttributeValue(std::move(value));
}

SpanAttributeType SpanAttributeValue::type() const noexcept {
    switch (m_storage.index()) {
    case 0:
        return SpanAttributeType::kInt64;
    case 1:
        return SpanAttributeType::kUInt64;
    case 2:
        return SpanAttributeType::kDouble;
    case 3:
        return SpanAttributeType::kBool;
    default:
        return SpanAttributeType::kString;
    }
}

std::int64_t SpanAttributeValue::asInt64() const {
    return std::get<std::int64_t>(m_storage);
}

std::uint64_t SpanAttributeValue::asUInt64() const {
    return std::get<std::uint64_t>(m_storage);
}

double SpanAttributeValue::asDouble() const {
    return std::get<double>(m_storage);
}

bool SpanAttributeValue::asBool() const {
    return std::get<bool>(m_storage);
}

const std::string& SpanAttributeValue::asString() const {
    return std::get<std::string>(m_storage);
}

SpanAttribute spanAttribute(std::string_view name, std::int64_t value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::fromInt64(value),
    };
}

SpanAttribute spanAttribute(std::string_view name, int value) {
    return spanAttribute(name, static_cast<std::int64_t>(value));
}

SpanAttribute spanAttribute(std::string_view name, std::uint64_t value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::fromUInt64(value),
    };
}

SpanAttribute spanAttribute(std::string_view name, double value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::fromDouble(value),
    };
}

SpanAttribute spanAttribute(std::string_view name, bool value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::fromBool(value),
    };
}

SpanAttribute spanAttribute(std::string_view name, std::string_view value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::fromString(std::string(value)),
    };
}

SpanAttribute spanAttribute(std::string_view name, const char* value) {
    return spanAttribute(name, std::string_view(value == nullptr ? "" : value));
}

void setSpanTimingPolicy(SpanTimingPolicy policy) noexcept {
    g_spanTimingPolicy.store(policy, std::memory_order_relaxed);
}

SpanTimingPolicy spanTimingPolicy() noexcept {
    return g_spanTimingPolicy.load(std::memory_order_relaxed);
}

Span::Span(std::string name, TraceContext context)
    : Span(std::move(name), std::move(context), spanTimingPolicy()) {
}

Span::Span(std::string name, TraceContext context, SpanTimingPolicy timingPolicy)
    : Span(std::move(name), SpanContext(context), context.tracestate(), timingPolicy) {
}

Span::Span(std::string name, SpanContext context, std::string tracestate)
    : Span(std::move(name), context, std::move(tracestate), spanTimingPolicy()) {
}

Span::Span(std::string name, SpanContext context, std::string tracestate, SpanTimingPolicy timingPolicy)
    : m_name(std::move(name)),
      m_context(std::move(context)),
      m_tracestate(std::move(tracestate)),
      m_startedAt(timingPolicy == SpanTimingPolicy::kEnabled ? Clock::now() : Clock::time_point{}) {
}

void Span::end() noexcept {
    if (!m_ended) {
        if (m_startedAt != Clock::time_point{}) {
            m_endedAt = Clock::now();
        }
        m_ended = true;
    }
}

void Span::setStatus(SpanStatusCode code, std::string message) {
    m_status = SpanStatus{.code = code, .message = std::move(message)};
}

bool Span::setAttribute(SpanAttribute attribute) {
    if (m_attributes.size() >= kMaxAttributes) {
        return false;
    }
    m_attributes.push_back(std::move(attribute));
    return true;
}

bool Span::setAttribute(std::string_view name, std::int64_t value) {
    return setAttribute(spanAttribute(name, value));
}

bool Span::setAttribute(std::string_view name, int value) {
    return setAttribute(name, static_cast<std::int64_t>(value));
}

bool Span::setAttribute(std::string_view name, std::uint64_t value) {
    return setAttribute(spanAttribute(name, value));
}

bool Span::setAttribute(std::string_view name, double value) {
    return setAttribute(spanAttribute(name, value));
}

bool Span::setAttribute(std::string_view name, bool value) {
    return setAttribute(spanAttribute(name, value));
}

bool Span::setAttribute(std::string_view name, std::string_view value) {
    return setAttribute(spanAttribute(name, value));
}

bool Span::setAttribute(std::string_view name, const char* value) {
    return setAttribute(name, std::string_view(value == nullptr ? "" : value));
}

} // namespace galay::tracing
