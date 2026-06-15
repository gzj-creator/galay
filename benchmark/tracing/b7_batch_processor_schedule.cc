#include "galay-tracing/kernel/batch_span_processor.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kSpanName = "batch-processor-benchmark-span";

class CountingExporter final : public galay::tracing::SpanExporter {
public:
    galay::tracing::ExportResult exportSpans(std::span<const galay::tracing::Span> spans) override {
        std::size_t observed = 0;
        for (const auto& span : spans) {
            observed += span.name().size();
            observed += span.tracestate().size();
            observed += span.attributes().size();
            observed += span.status().message.size();
            observed += span.spanContext().traceId().bytes().size();
            observed += span.spanContext().spanId().bytes().size();
        }
        observed_weight.fetch_add(observed, std::memory_order_relaxed);
        exported_spans.fetch_add(spans.size(), std::memory_order_relaxed);
        export_calls.fetch_add(1, std::memory_order_relaxed);
        return galay::tracing::ExportResult::kSuccess;
    }

    bool forceFlush(std::chrono::milliseconds) override {
        return true;
    }

    std::atomic<std::size_t> exported_spans{0};
    std::atomic<std::size_t> export_calls{0};
    std::atomic<std::size_t> observed_weight{0};
};

[[nodiscard]] const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

[[nodiscard]] const char* modeName(galay::tracing::BatchSpanScheduleMode mode) {
    switch (mode) {
    case galay::tracing::BatchSpanScheduleMode::kTimed:
        return "timed";
    case galay::tracing::BatchSpanScheduleMode::kOnEnd:
        return "on_end";
    case galay::tracing::BatchSpanScheduleMode::kBatchSize:
        return "batch_size";
    }
    return "unknown";
}

[[nodiscard]] galay::tracing::TraceContext makeContext() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
}

std::vector<galay::tracing::Span> makeSpans(std::size_t count) {
    std::vector<galay::tracing::Span> spans;
    spans.reserve(count);
    const auto context = makeContext();
    for (std::size_t i = 0; i < count; ++i) {
        galay::tracing::Span span(std::string(kSpanName), context);
        span.end();
        spans.push_back(std::move(span));
    }
    return spans;
}

struct ScheduleResult {
    double ns_per_on_end;
    double ns_per_flush;
    double ns_per_e2e;
};

ScheduleResult runScheduleOnce(galay::tracing::BatchSpanScheduleMode mode) {
    constexpr std::size_t kIterations = 100000;
    constexpr std::size_t kBatchSize = 512;
    auto spans = makeSpans(kIterations);
    auto exporter = std::make_unique<CountingExporter>();
    auto* rawExporter = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), {
        .queue_capacity = kIterations,
        .max_batch_size = kBatchSize,
        .flush_interval = std::chrono::hours(1),
        .schedule_mode = mode,
    });

    const auto start = std::chrono::steady_clock::now();
    for (auto& span : spans) {
        processor.onEnd(std::move(span));
    }
    const auto afterOnEnd = std::chrono::steady_clock::now();
    const bool flushed = processor.forceFlush(std::chrono::seconds(30));
    const auto afterFlush = std::chrono::steady_clock::now();
    const bool shutdown = processor.shutdown(std::chrono::seconds(30));

    const auto onEndNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterOnEnd - start).count();
    const auto flushNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterFlush - afterOnEnd).count();
    const auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterFlush - start).count();
    const auto exported = rawExporter->exported_spans.load(std::memory_order_relaxed);
    const auto exportCalls = rawExporter->export_calls.load(std::memory_order_relaxed);
    const auto observedWeight = rawExporter->observed_weight.load(std::memory_order_relaxed);
    const auto dropped = processor.droppedSpanCount();
    const bool ok = flushed && shutdown && exported == kIterations && dropped == 0;

    if (!ok) {
        std::cout << "B7-BatchProcessorSchedule workload=" << kIterations
                  << " build=" << buildType()
                  << " mode=" << modeName(mode)
                  << " ok=0 export_calls=" << exportCalls
                  << " exported=" << exported
                  << " observed_weight=" << observedWeight
                  << " dropped=" << dropped << '\n';
    }
    return {
        .ns_per_on_end = static_cast<double>(onEndNs) / kIterations,
        .ns_per_flush = static_cast<double>(flushNs) / kIterations,
        .ns_per_e2e = static_cast<double>(totalNs) / kIterations,
    };
}

void runSchedule(galay::tracing::BatchSpanScheduleMode mode) {
    constexpr std::size_t kIterations = 100000;
    constexpr std::size_t kBatchSize = 512;
    constexpr std::size_t kRounds = 7;
    std::array<ScheduleResult, kRounds> results{};
    for (auto& result : results) {
        result = runScheduleOnce(mode);
    }

    auto best = [](std::array<ScheduleResult, kRounds> values, auto member) {
        return std::ranges::min(values, {}, member).*member;
    };
    auto median = [](std::array<ScheduleResult, kRounds> values, auto member) {
        std::ranges::sort(values, {}, member);
        return values[kRounds / 2].*member;
    };

    std::cout << "B7-BatchProcessorSchedule workload=" << kIterations
              << " build=" << buildType()
              << " mode=" << modeName(mode)
              << " rounds=" << kRounds
              << " batch_size=" << kBatchSize
              << " queue_capacity=" << kIterations
              << " payload_name_len=" << kSpanName.size()
              << " best_on_end_ns=" << best(results, &ScheduleResult::ns_per_on_end)
              << " median_on_end_ns=" << median(results, &ScheduleResult::ns_per_on_end)
              << " best_flush_ns=" << best(results, &ScheduleResult::ns_per_flush)
              << " median_flush_ns=" << median(results, &ScheduleResult::ns_per_flush)
              << " best_e2e_ns=" << best(results, &ScheduleResult::ns_per_e2e)
              << " median_e2e_ns=" << median(results, &ScheduleResult::ns_per_e2e)
              << '\n';
}

} // namespace

int main() {
    runSchedule(galay::tracing::BatchSpanScheduleMode::kTimed);
    runSchedule(galay::tracing::BatchSpanScheduleMode::kOnEnd);
    runSchedule(galay::tracing::BatchSpanScheduleMode::kBatchSize);
}
