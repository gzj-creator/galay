/**
 * @file span.cc
 * @brief Span 核心数据类型与属性操作实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 SpanAttributeValue 的类型安全存取、SpanAttribute 工厂函数、
 * 全局 Span 时间策略管理，以及 Span 的构造、属性设置和生命周期控制。
 */

#include "span.h"

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

SpanAttributeValue SpanAttributeValue::from_int64(std::int64_t value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::from_uint64(std::uint64_t value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::from_double(double value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::from_bool(bool value) {
    return SpanAttributeValue(value);
}

SpanAttributeValue SpanAttributeValue::from_string(std::string value) {
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

std::int64_t SpanAttributeValue::as_int64() const {
    return std::get<std::int64_t>(m_storage);
}

std::uint64_t SpanAttributeValue::as_uint64() const {
    return std::get<std::uint64_t>(m_storage);
}

double SpanAttributeValue::as_double() const {
    return std::get<double>(m_storage);
}

bool SpanAttributeValue::as_bool() const {
    return std::get<bool>(m_storage);
}

const std::string& SpanAttributeValue::as_string() const {
    return std::get<std::string>(m_storage);
}

SpanAttribute span_attribute(std::string_view name, std::int64_t value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::from_int64(value),
    };
}

SpanAttribute span_attribute(std::string_view name, int value) {
    return span_attribute(name, static_cast<std::int64_t>(value));
}

SpanAttribute span_attribute(std::string_view name, std::uint64_t value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::from_uint64(value),
    };
}

SpanAttribute span_attribute(std::string_view name, double value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::from_double(value),
    };
}

SpanAttribute span_attribute(std::string_view name, bool value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::from_bool(value),
    };
}

SpanAttribute span_attribute(std::string_view name, std::string_view value) {
    return SpanAttribute{
        .name = std::string(name),
        .value = SpanAttributeValue::from_string(std::string(value)),
    };
}

SpanAttribute span_attribute(std::string_view name, const char* value) {
    return span_attribute(name, std::string_view(value == nullptr ? "" : value));
}

void set_span_timing_policy(SpanTimingPolicy policy) noexcept {
    g_spanTimingPolicy.store(policy, std::memory_order_relaxed);
}

SpanTimingPolicy span_timing_policy() noexcept {
    return g_spanTimingPolicy.load(std::memory_order_relaxed);
}

Span::Span(std::string name, TraceContext context)
    : Span(std::move(name), std::move(context), span_timing_policy()) {
}

Span::Span(std::string name, TraceContext context, SpanTimingPolicy timingPolicy)
    : Span(std::move(name), SpanContext(context), context.tracestate(), timingPolicy) {
}

Span::Span(std::string name, SpanContext context, std::string tracestate)
    : Span(std::move(name), context, std::move(tracestate), span_timing_policy()) {
}

Span::Span(std::string name, SpanContext context, std::string tracestate, SpanTimingPolicy timingPolicy)
    : m_name(std::move(name)),
      m_tracestate(std::move(tracestate)),
      m_startedAt(timingPolicy == SpanTimingPolicy::kEnabled ? Clock::now() : Clock::time_point{}),
      m_context(std::move(context)) {
}

Span Span::clone() const {
    return Span(*this);
}

void Span::end() noexcept {
    if (!m_ended) {
        if (m_startedAt != Clock::time_point{}) {
            m_endedAt = Clock::now();
        }
        m_ended = true;
    }
}

void Span::set_status(SpanStatusCode code, std::string message) {
    m_status = SpanStatus{.message = std::move(message), .code = code};
}

bool Span::set_attribute(SpanAttribute attribute) {
    if (m_attributes.size() >= kMaxAttributes) {
        return false;
    }
    m_attributes.push_back(std::move(attribute));
    return true;
}

bool Span::set_attribute(std::string_view name, std::int64_t value) {
    return set_attribute(span_attribute(name, value));
}

bool Span::set_attribute(std::string_view name, int value) {
    return set_attribute(name, static_cast<std::int64_t>(value));
}

bool Span::set_attribute(std::string_view name, std::uint64_t value) {
    return set_attribute(span_attribute(name, value));
}

bool Span::set_attribute(std::string_view name, double value) {
    return set_attribute(span_attribute(name, value));
}

bool Span::set_attribute(std::string_view name, bool value) {
    return set_attribute(span_attribute(name, value));
}

bool Span::set_attribute(std::string_view name, std::string_view value) {
    return set_attribute(span_attribute(name, value));
}

bool Span::set_attribute(std::string_view name, const char* value) {
    return set_attribute(name, std::string_view(value == nullptr ? "" : value));
}

bool Span::add_event(std::string_view name, std::vector<SpanAttribute> attributes) {
    if (m_events.size() >= kMaxEvents) {
        return false;
    }
    if (attributes.size() > kMaxEventAttributes) {
        attributes.resize(kMaxEventAttributes);
    }
    m_events.push_back(SpanEvent{
        .name = std::string(name),
        .attributes = std::move(attributes),
        .timestamp = m_startedAt == Clock::time_point{} ? Clock::time_point{} : Clock::now(),
    });
    return true;
}

bool Span::add_link(SpanContext context, std::string tracestate, std::vector<SpanAttribute> attributes) {
    if (m_links.size() >= kMaxLinks) {
        return false;
    }
    if (attributes.size() > kMaxLinkAttributes) {
        attributes.resize(kMaxLinkAttributes);
    }
    m_links.push_back(SpanLink{
        .tracestate = std::move(tracestate),
        .attributes = std::move(attributes),
        .context = std::move(context),
    });
    return true;
}

} // namespace galay::tracing
