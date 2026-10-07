#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-tracing/context/context_storage.h>
#include <galay/cpp/galay-tracing/kernel/span_guard.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string_view>

namespace {

template <typename T>
void do_not_optimize(const T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    (void)value;
#endif
}

[[nodiscard]] const char* build_type() {
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
        do_not_optimize(m_id);
    }

private:
    std::uint64_t m_id;
};

class SubscriberLikeNoopTracer {
public:
    [[nodiscard]] SubscriberLikeNoopSpan start_span(std::string_view name) noexcept {
        do_not_optimize(name);
        return SubscriberLikeNoopSpan(m_nextId.fetch_add(1, std::memory_order_relaxed));
    }

private:
    std::atomic<std::uint64_t> m_nextId{1};
};

class SpanTimingPolicyScope {
public:
    explicit SpanTimingPolicyScope(galay::tracing::SpanTimingPolicy policy) noexcept
        : m_previous(galay::tracing::span_timing_policy()) {
        galay::tracing::set_span_timing_policy(policy);
    }

    ~SpanTimingPolicyScope() {
        galay::tracing::set_span_timing_policy(m_previous);
    }

private:
    galay::tracing::SpanTimingPolicy m_previous;
};

double measure_subscriber_like_noop_ns() {
    constexpr int kIterations = 20000;
    SubscriberLikeNoopTracer tracer;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        auto span = tracer.start_span("bench");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

double measure_span_id_random_ns() {
    constexpr int kIterations = 20000;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto id = galay::tracing::SpanId::random();
        do_not_optimize(id);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

double measure_trace_id_random_ns() {
    constexpr int kIterations = 20000;

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto id = galay::tracing::TraceId::random();
        do_not_optimize(id);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

double measure_child_span_ns(bool sampled, galay::tracing::SpanTimingPolicy timingPolicy) {
    constexpr int kIterations = 20000;
    SpanTimingPolicyScope timing(timingPolicy);
    const auto parent = galay::tracing::TraceContext(
        galay::tracing::TraceId::random(),
        galay::tracing::SpanId::random(),
        sampled ? 0x01 : 0x00);
    galay::tracing::set_current_context(parent);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto span = galay::tracing::start_span("bench");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::clear_current_context();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / kIterations;
}

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    std::cout << "B3-SpanScope workload=20000 build=" << build_type() << " backend=core"
              << " subscriber_like_noop_ns_per_scope=" << measure_subscriber_like_noop_ns()
              << " span_id_random_ns=" << measure_span_id_random_ns()
              << " trace_id_random_ns=" << measure_trace_id_random_ns()
              << " sampled_ns_per_scope=" << measure_child_span_ns(true, galay::tracing::SpanTimingPolicy::kDisabled)
              << " unsampled_ns_per_scope=" << measure_child_span_ns(false, galay::tracing::SpanTimingPolicy::kDisabled)
              << " sampled_timing_enabled_ns_per_scope="
              << measure_child_span_ns(true, galay::tracing::SpanTimingPolicy::kEnabled) << '\n';
}
