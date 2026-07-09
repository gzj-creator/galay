#include <galay/cpp/galay-tracing/kernel/batch_span_processor.h>
#include <galay/cpp/galay-tracing/kernel/span_exporter.h>

#include <cassert>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class RecordingExporter final : public galay::tracing::SpanExporter {
public:
    galay::tracing::ExportResult exportSpans(std::span<const galay::tracing::Span> spans) override {
        for (const auto& span : spans) {
            exported.push_back(span.clone());
        }
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

class BlockingExporter final : public galay::tracing::SpanExporter {
public:
    galay::tracing::ExportResult exportSpans(std::span<const galay::tracing::Span>) override {
        export_started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return galay::tracing::ExportResult::kSuccess;
    }

    std::atomic<bool> export_started{false};
};

class BlockingFirstExporter final : public galay::tracing::SpanExporter {
public:
    galay::tracing::ExportResult exportSpans(std::span<const galay::tracing::Span>) override {
        const auto call = export_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (call == 1) {
            export_started.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!release_first.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() <= deadline) {
                std::this_thread::yield();
            }
        }
        return galay::tracing::ExportResult::kSuccess;
    }

    std::atomic<std::size_t> export_calls{0};
    std::atomic<bool> export_started{false};
    std::atomic<bool> release_first{false};
};

class SlowExporter final : public galay::tracing::SpanExporter {
public:
    explicit SlowExporter(std::chrono::milliseconds delay)
        : delay(delay) {}

    galay::tracing::ExportResult exportSpans(std::span<const galay::tracing::Span>) override {
        export_started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(delay);
        return galay::tracing::ExportResult::kSuccess;
    }

    std::chrono::milliseconds delay;
    std::atomic<bool> export_started{false};
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

galay::tracing::BatchSpanProcessorConfig test_config() {
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
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), test_config());

    processor.onEnd(makeSpan("sampled", true));

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.size() == 1);
    assert(raw->exported[0].name() == "sampled");
}

void unsampledSpansAreNotExported() {
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), test_config());

    processor.onEnd(makeSpan("unsampled", false));

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.empty());
}

void fullQueueDropsNewSpans() {
    auto config = test_config();
    config.queue_capacity = 1;
    config.max_batch_size = 8;
    config.schedule_mode = galay::tracing::BatchSpanScheduleMode::kTimed;
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
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), test_config());

    processor.onEnd(makeSpan("one"));
    processor.onEnd(makeSpan("two"));

    assert(processor.forceFlush(std::chrono::seconds(1)));
    assert(raw->exported.size() == 2);
}

void timedScheduleDoesNotWakeOnEverySpan() {
    auto config = test_config();
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
    auto config = test_config();
    config.schedule_mode = galay::tracing::BatchSpanScheduleMode::kOnEnd;
    config.flush_interval = std::chrono::hours(1);
    auto exporter = std::make_unique<RecordingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("on-end"));

    assert(waitForExportedSize(*raw, 1));
}

void batchScheduleWakesWhenThresholdReached() {
    auto config = test_config();
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
    auto config = test_config();
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
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), test_config());

    processor.onEnd(makeSpan("before-shutdown"));

    assert(processor.shutdown(std::chrono::seconds(1)));
    assert(raw->shutdown_called);
    assert(raw->exported.size() == 1);

    processor.onEnd(makeSpan("after-shutdown"));
    assert(raw->exported.size() == 1);
}

void shutdownHonorsTimeoutWhileWorkerIsExporting() {
    auto config = test_config();
    config.queue_capacity = 1;
    config.max_batch_size = 1;
    config.flush_interval = std::chrono::hours(1);

    auto exporter = std::make_unique<BlockingExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("slow"));
    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!raw->export_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <= waitDeadline) {
        std::this_thread::yield();
    }
    assert(raw->export_started.load(std::memory_order_acquire));

    const auto start = std::chrono::steady_clock::now();
    assert(!processor.shutdown(std::chrono::milliseconds(10)));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(100));
}

void shutdownDeadlineStopsFurtherDrainAfterTimedOutExport() {
    auto config = test_config();
    config.queue_capacity = 4;
    config.max_batch_size = 1;
    config.flush_interval = std::chrono::hours(1);
    config.schedule_mode = galay::tracing::BatchSpanScheduleMode::kBatchSize;

    auto exporter = std::make_unique<BlockingFirstExporter>();
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), config);

    processor.onEnd(makeSpan("blocked"));
    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!raw->export_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <= waitDeadline) {
        std::this_thread::yield();
    }
    assert(raw->export_started.load(std::memory_order_acquire));

    processor.onEnd(makeSpan("queued-after-timeout"));
    processor.onEnd(makeSpan("also-queued-after-timeout"));
    assert(!processor.shutdown(std::chrono::milliseconds(10)));

    raw->release_first.store(true, std::memory_order_release);
    const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (raw->export_calls.load(std::memory_order_acquire) == 1 &&
           std::chrono::steady_clock::now() <= settleDeadline) {
        std::this_thread::yield();
    }
    assert(raw->export_calls.load(std::memory_order_acquire) == 1);
}

int runSlowExporterDestructorCase()
{
    auto exporter = std::make_unique<SlowExporter>(std::chrono::milliseconds(700));
    auto* raw = exporter.get();
    galay::tracing::BatchSpanProcessor processor(std::move(exporter), {
        .queue_capacity = 1,
        .max_batch_size = 1,
        .flush_interval = std::chrono::hours(1),
        .schedule_mode = galay::tracing::BatchSpanScheduleMode::kBatchSize,
    });

    processor.onEnd(makeSpan("slow-destructor"));
    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!raw->export_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <= waitDeadline) {
        std::this_thread::yield();
    }
    assert(raw->export_started.load(std::memory_order_acquire));
    assert(!processor.shutdown(std::chrono::milliseconds(10)));
    return 0;
}

void destructorWaitsForSlowExporterInsteadOfTerminating()
{
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _exit(runSlowExporterDestructorCase());
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
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
    shutdownHonorsTimeoutWhileWorkerIsExporting();
    shutdownDeadlineStopsFurtherDrainAfterTimedOutExport();
    destructorWaitsForSlowExporterInsteadOfTerminating();
}
