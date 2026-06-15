#include "galay-tracing/context/context_storage.h"
#include "galay-tracing/kernel/span_guard.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string_view>

namespace {

template <typename T>
void doNotOptimize(const T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    (void)value;
#endif
}

[[nodiscard]] const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

class SubscriberLikeNoopSpan {
public:
    explicit SubscriberLikeNoopSpan(std::uint64_t id) noexcept
        : m_id(id) {
    }

    ~SubscriberLikeNoopSpan() noexcept {
        doNotOptimize(m_id);
    }

private:
    std::uint64_t m_id;
};

class SubscriberLikeNoopTracer {
public:
    [[nodiscard]] SubscriberLikeNoopSpan startSpan(std::string_view name) noexcept {
        doNotOptimize(name);
        return SubscriberLikeNoopSpan(m_nextId.fetch_add(1, std::memory_order_relaxed));
    }

private:
    std::atomic<std::uint64_t> m_nextId{1};
};

class SpanTimingPolicyScope {
public:
    explicit SpanTimingPolicyScope(galay::tracing::SpanTimingPolicy policy) noexcept
        : m_previous(galay::tracing::spanTimingPolicy()) {
        galay::tracing::setSpanTimingPolicy(policy);
    }

    ~SpanTimingPolicyScope() {
        galay::tracing::setSpanTimingPolicy(m_previous);
    }

private:
    galay::tracing::SpanTimingPolicy m_previous;
};

double measureSubscriberLikeNoopNs() {
    constexpr int kIterations = 20000;
    SubscriberLikeNoopTracer tracer;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        auto span = tracer.startSpan("bench");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

double measureSpanIdRandomNs() {
    constexpr int kIterations = 20000;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto id = galay::tracing::SpanId::random();
        doNotOptimize(id);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

double measureTraceIdRandomNs() {
    constexpr int kIterations = 20000;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto id = galay::tracing::TraceId::random();
        doNotOptimize(id);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

double measureChildSpanNs(bool sampled, galay::tracing::SpanTimingPolicy timingPolicy) {
    constexpr int kIterations = 20000;
    SpanTimingPolicyScope timing(timingPolicy);
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::random(),
        galay::tracing::SpanId::random(),
        sampled ? 0x01 : 0x00);
    galay::tracing::setCurrentContext(parent);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto span = galay::tracing::startSpan("bench");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::clearCurrentContext();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

} // namespace

int main() {
    std::cout << "B3-SpanScope workload=20000 build=" << buildType() << " backend=core"
              << " subscriber_like_noop_ns_per_scope=" << measureSubscriberLikeNoopNs()
              << " span_id_random_ns=" << measureSpanIdRandomNs()
              << " trace_id_random_ns=" << measureTraceIdRandomNs()
              << " sampled_ns_per_scope=" << measureChildSpanNs(true, galay::tracing::SpanTimingPolicy::kDisabled)
              << " unsampled_ns_per_scope=" << measureChildSpanNs(false, galay::tracing::SpanTimingPolicy::kDisabled)
              << " sampled_timing_enabled_ns_per_scope="
              << measureChildSpanNs(true, galay::tracing::SpanTimingPolicy::kEnabled) << '\n';
}
