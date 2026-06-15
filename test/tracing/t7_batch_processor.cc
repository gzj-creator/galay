#include "galay-tracing/kernel/batch_span_processor.h"
#include "galay-tracing/kernel/span_exporter.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class RecordingExporter final : public galay::tracing::SpanExporter {
public:
    galay::tracing::ExportResult exportSpans(std::span<const galay::tracing::Span> spans) override {
        exported.insert(exported.end(), spans.begin(), spans.end());
        exported_count.fetch_add(spans.size(), std::memory_order_release);
        return galay::tracing::ExportResult::kSuccess;
    }

    bool shutdown(std::chrono::milliseconds) override {
        shutdown_called = true;
        return true;
    }

    [[nodiscard]] std::size_t exportedSize() const {
        return exported_count.load(std::memory_order_acquire);
    }

    std::vector<galay::tracing::Span> exported;
    std::atomic<std::size_t> exported_count{0};
    bool shutdown_called{false};
};

galay::tracing::Span makeSpan(std::string_view name, bool sampled = true) {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::random(),
        galay::tracing::SpanId::random(),
        sampled ? 0x01 : 0x00);
    galay::tracing::Span span(std::string(name), context);
    span.end();
    return span;
}

galay::tracing::BatchSpanProcessorConfig testConfig() {
    return {
        .queue_capacity = 8,
        .max_batch_size = 8,
        .flush_interval = std::chrono::hours(1),
    };
}

bool waitForExportedSize(const RecordingExporter& exporter, std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() <= deadline) {
        if (exporter.exportedSize() >= expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void sampledSpansAreExported() {
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), testConfig());

    processor.onEnd(makeSpan("sampled", true));

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.size() == 1);
    assert(raw->exported[0].name() == "sampled");
}

void unsampledSpansAreNotExported() {
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), testConfig());

    processor.onEnd(makeSpan("unsampled", false));

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.empty());
}

void fullQueueDropsNewSpans() {
    auto config = testConfig();
    config.queue_capacity = 1;
    config.max_batch_size = 8;
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("first"));
    processor.onEnd(makeSpan("second"));

    assert(processor.droppedSpanCount() == 1);
    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.size() == 1);
    assert(raw->exported[0].name() == "first");
}

void forceFlushExportsQueuedSpans() {
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), testConfig());

    processor.onEnd(makeSpan("one"));
    processor.onEnd(makeSpan("two"));

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.size() == 2);
}

void timedScheduleDoesNotWakeOnEverySpan() {
    auto config = testConfig();
    config.schedule_mode = galay::tracing::BatchSpanScheduleMode::kTimed;
    config.flush_interval = std::chrono::hours(1);
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("timed"));

    assert(!waitForExportedSize(*raw, 1));
    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exportedSize() == 1);
}

void onEndScheduleWakesForEachSpan() {
    auto config = testConfig();
    config.schedule_mode = galay::tracing::BatchSpanScheduleMode::kOnEnd;
    config.flush_interval = std::chrono::hours(1);
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("on-end"));

    assert(waitForExportedSize(*raw, 1));
}

void batchScheduleWakesWhenThresholdReached() {
    auto config = testConfig();
    config.schedule_mode = galay::tracing::BatchSpanScheduleMode::kBatchSize;
    config.max_batch_size = 3;
    config.flush_interval = std::chrono::hours(1);
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("one"));
    processor.onEnd(makeSpan("two"));
    assert(!waitForExportedSize(*raw, 1));

    processor.onEnd(makeSpan("three"));
    assert(waitForExportedSize(*raw, 3));
}

void concurrentOnEndFlushesAllSampledSpans() {
    constexpr auto kThreadCount = 4;
    constexpr auto kSpansPerThread = 64;
    auto config = testConfig();
    config.queue_capacity = kThreadCount * kSpansPerThread;
    config.max_batch_size = 16;
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
        producers.emplace_back([&processor, threadIndex] {
            for (int spanIndex = 0; spanIndex < kSpansPerThread; ++spanIndex) {
                processor.onEnd(makeSpan("span"));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exportedSize() == kThreadCount * kSpansPerThread);
    assert(processor.droppedSpanCount() == 0);
}

void shutdownFlushesAndStops() {
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), testConfig());

    processor.onEnd(makeSpan("before-shutdown"));

    assert(processor.shutdown(std::chrono::seconds(1)));
    assert(raw->shutdown_called);
    assert(raw->exported.size() == 1);

    processor.onEnd(makeSpan("after-shutdown"));
    assert(raw->exported.size() == 1);
}

} // namespace

int main() {
    sampledSpansAreExported();
    unsampledSpansAreNotExported();
    fullQueueDropsNewSpans();
    forceFlushExportsQueuedSpans();
    timedScheduleDoesNotWakeOnEverySpan();
    onEndScheduleWakesForEachSpan();
    batchScheduleWakesWhenThresholdReached();
    concurrentOnEndFlushesAllSampledSpans();
    shutdownFlushesAndStops();
}
