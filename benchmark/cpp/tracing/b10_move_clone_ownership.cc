#include <galay/cpp/galay-tracing/kernel/otlp_http_exporter.h>
#include <galay/cpp/galay-tracing/log/console_sink.h>
#include <galay/cpp/galay-tracing/log/logger.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] std::size_t iterationCount(int argc, char** argv) noexcept {
    if (argc < 2) {
        return 50000;
    }

    const auto parsed = std::strtoull(argv[1], nullptr, 10);
    return std::max<std::size_t>(static_cast<std::size_t>(parsed), 1);
}

[[nodiscard]] galay::tracing::TraceContext makeContext(std::string spanId = "00f067aa0ba902b7") {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex(spanId),
        0x01,
        "vendor=value");
    context.setParentSpanId(galay::tracing::SpanId::fromHex("1111111111111111"));
    return context;
}

[[nodiscard]] galay::tracing::Span makeSpan(std::string name = "ownership-bench-span") {
    galay::tracing::Span span(std::move(name), makeContext());
    span.setKind(galay::tracing::SpanKind::kClient);
    span.setStatus(galay::tracing::SpanStatusCode::kError, "timeout");
    bool ok = true;
    ok = span.setAttribute("http.method", "GET") && ok;
    ok = span.setAttribute("http.status_code", 503) && ok;
    ok = span.addEvent("retry", {galay::tracing::spanAttribute("attempt", 1)}) && ok;
    ok = span.addLink(
        galay::tracing::SpanContext(makeContext("2222222222222222")),
        "linked=1",
        {galay::tracing::spanAttribute("link.kind", "batch")}) && ok;
    if (!ok) {
        span.setStatus(galay::tracing::SpanStatusCode::kError, "benchmark span setup failed");
    }
    span.end();
    return span;
}

[[nodiscard]] galay::tracing::LogRecord makeLogRecord() {
    return galay::tracing::LogRecord(
        galay::tracing::LogLevel::kWarn,
        "ownership benchmark message",
        {"benchmark/cpp/tracing/b10_move_clone_ownership.cc", 0, "makeLogRecord"},
        galay::tracing::makeLogContext(makeContext()));
}

class NullSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord&) override {
        ++writes;
    }

    std::size_t writes{0};
};

[[nodiscard]] galay::tracing::Logger::SinkSnapshot makeSnapshot() {
    galay::tracing::Logger::SinkSnapshot snapshot;
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    return snapshot;
}

template <typename Fn>
[[nodiscard]] double measureNsPerOp(std::size_t iterations, Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        fn(i);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return static_cast<double>(ns) / static_cast<double>(iterations);
}

} // namespace

int main(int argc, char** argv) {
    const auto iterations = iterationCount(argc, argv);
    std::size_t observed = 0;

    const auto sourceSpan = makeSpan();
    const auto spanCloneNs = measureNsPerOp(iterations, [&](std::size_t) {
        auto cloned = sourceSpan.clone();
        observed += cloned.attributes().size() + cloned.events().size() + cloned.links().size();
        doNotOptimize(cloned);
    });

    const auto spanMoveNs = measureNsPerOp(iterations, [&](std::size_t i) {
        std::vector<galay::tracing::Span> spans;
        spans.reserve(1);
        spans.push_back(makeSpan("move-span-" + std::to_string(i)));
        observed += spans.front().name().size();
        doNotOptimize(spans);
    });

    const auto sourceRecord = makeLogRecord();
    const auto logCloneNs = measureNsPerOp(iterations, [&](std::size_t) {
        auto cloned = sourceRecord.clone();
        observed += cloned.message.size();
        doNotOptimize(cloned);
    });

    const auto logMoveNs = measureNsPerOp(iterations, [&](std::size_t) {
        std::vector<galay::tracing::LogRecord> records;
        records.reserve(1);
        records.push_back(makeLogRecord());
        observed += records.front().message.size();
        doNotOptimize(records);
    });

    const auto sourceSnapshot = makeSnapshot();
    const auto snapshotCloneNs = measureNsPerOp(iterations, [&](std::size_t) {
        auto cloned = sourceSnapshot.clone();
        observed += cloned.sinks.size();
        doNotOptimize(cloned);
    });

    const auto snapshotMoveNs = measureNsPerOp(iterations, [&](std::size_t) {
        std::vector<galay::tracing::Logger::SinkSnapshot> snapshots;
        snapshots.reserve(1);
        snapshots.push_back(makeSnapshot());
        observed += snapshots.front().sinks.size();
        doNotOptimize(snapshots);
    });

    const auto exporterConstructNs = measureNsPerOp(iterations, [&](std::size_t) {
        galay::tracing::OtlpHttpExporter exporter({}, [](galay::tracing::OtlpHttpRequest) {
            return galay::tracing::OtlpHttpResponse{.status_code = 200};
        });
        doNotOptimize(exporter);
    });

    const auto sinkConstructNs = measureNsPerOp(iterations, [&](std::size_t) {
        galay::tracing::ConsoleSink sink;
        doNotOptimize(sink);
    });

    std::cout << "B10-MoveCloneOwnership workload=" << iterations
              << " build=" << buildType()
              << " span_clone_ns=" << spanCloneNs
              << " span_move_ns=" << spanMoveNs
              << " log_clone_ns=" << logCloneNs
              << " log_move_ns=" << logMoveNs
              << " snapshot_clone_ns=" << snapshotCloneNs
              << " snapshot_move_ns=" << snapshotMoveNs
              << " exporter_construct_ns=" << exporterConstructNs
              << " sink_construct_ns=" << sinkConstructNs
              << " observed=" << observed << '\n';
}
