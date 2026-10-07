/**
 * @file sampler.cc
 * @brief 追踪采样器接口与内置实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现四种内置采样策略：始终采样、始终不采样、基于父 Span 采样决策
 * 和基于 TraceId 高 64 位比例的采样。通过原子指针支持全局采样器热替换。
 */

#include "sampler.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace galay::tracing {

namespace {

std::atomic<const Sampler*> g_sampler{nullptr};

[[nodiscard]] std::uint64_t trace_id_high_bits(const TraceId& trace_id) noexcept {
    static_assert(TraceId::kByteLength >= sizeof(std::uint64_t));
    const auto& bytes = trace_id.bytes();
    return (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[0])) << 56U)
        | (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[1])) << 48U)
        | (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[2])) << 40U)
        | (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[3])) << 32U)
        | (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[4])) << 24U)
        | (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[5])) << 16U)
        | (static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[6])) << 8U)
        | static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[7]));
}

[[nodiscard]] const Sampler& built_in_sampler() noexcept {
    static const AlwaysOnSampler rootSampler;
    static const ParentBasedSampler sampler(rootSampler);
    return sampler;
}

} // namespace

bool AlwaysOnSampler::should_sample(const SpanContext*, const TraceId&) const noexcept {
    return true;
}

bool AlwaysOffSampler::should_sample(const SpanContext*, const TraceId&) const noexcept {
    return false;
}

ParentBasedSampler::ParentBasedSampler(const Sampler& rootSampler) noexcept
    : m_rootSampler(&rootSampler) {
}

bool ParentBasedSampler::should_sample(const SpanContext* parent, const TraceId& trace_id) const noexcept {
    if (parent != nullptr && parent->is_valid()) {
        return parent->sampled();
    }
    return m_rootSampler == nullptr || m_rootSampler->should_sample(nullptr, trace_id);
}

TraceIdRatioSampler::TraceIdRatioSampler(double ratio) noexcept
    : m_ratio(std::clamp(ratio, 0.0, 1.0)) {
}

bool TraceIdRatioSampler::should_sample(const SpanContext*, const TraceId& trace_id) const noexcept {
    if (m_ratio <= 0.0) {
        return false;
    }
    if (m_ratio >= 1.0) {
        return true;
    }

    constexpr long double kDenominator = static_cast<long double>(UINT64_MAX) + 1.0L;
    const auto normalized = static_cast<long double>(trace_id_high_bits(trace_id)) / kDenominator;
    return normalized < static_cast<long double>(m_ratio);
}

void set_sampler(const Sampler* sampler) noexcept {
    g_sampler.store(sampler, std::memory_order_release);
}

const Sampler& current_sampler() noexcept {
    if (auto* sampler = g_sampler.load(std::memory_order_acquire); sampler != nullptr) {
        return *sampler;
    }
    return built_in_sampler();
}

} // namespace galay::tracing
