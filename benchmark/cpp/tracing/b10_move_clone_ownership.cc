#include "../common/benchmark_environment.h"

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

[[nodiscard]] std::size_t iteration_count(int argc, char** argv) noexcept {
    if (argc < 2) {
        return 50000;
    }

    const auto parsed = std::strtoull(argv[1], nullptr, 10);
    return std::max<std::size_t>(static_cast<std::size_t>(parsed), 1);
}

[[nodiscard]] galay::tracing::TraceContext make_context(std::string spanId = "00f067aa0ba902b7") {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex(spanId),
        0x01,
        "vendor=value");
    context.set_parent_span_id(galay::tracing::SpanId::from_hex("1111111111111111"));
    return context;
}

[[nodiscard]] galay::tracing::Span make_span(std::string name = "ownership-bench-span") {
    galay::tracing::Span span(std::move(name), make_context());
    span.set_kind(galay::tracing::SpanKind::kClient);
    span.set_status(galay::tracing::SpanStatusCode::kError, "timeout");
    bool ok = true;
    ok = span.set_attribute("http.method", "GET") && ok;
    ok = span.set_attribute("http.status_code", 503) && ok;
    ok = span.add_event("retry", {galay::tracing::span_attribute("attempt", 1)}) && ok;
    ok = span.add_link(
        galay::tracing::SpanContext(make_context("2222222222222222")),
        "linked=1",
        {galay::tracing::span_attribute("link.kind", "batch")}) && ok;
    if (!ok) {
        span.set_status(galay::tracing::SpanStatusCode::kError, "benchmark span setup failed");
    }
    span.end();
    return span;
}

[[nodiscard]] galay::tracing::LogRecord make_log_record() {
    return galay::tracing::LogRecord(
        galay::tracing::LogLevel::kWarn,
        "ownership benchmark message",
        {"benchmark/cpp/tracing/b10_move_clone_ownership.cc", 0, "makeLogRecord"},
        galay::tracing::make_log_context(make_context()));
}

class NullSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord&) override {
        ++writes;
    }

    std::size_t writes{0};
};

[[nodiscard]] galay::tracing::Logger::SinkSnapshot make_snapshot() {
    galay::tracing::Logger::SinkSnapshot snapshot;
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    snapshot.sinks.push_back(std::make_shared<NullSink>());
    return snapshot;
}

template <typename Fn>
[[nodiscard]] double measure_ns_per_op(std::size_t iterations, Fn&& fn) {
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
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    const auto iterations = iteration_count(argc, argv);
    std::size_t observed = 0;

    const auto sourceSpan = make_span();
    const auto spanCloneNs = measure_ns_per_op(iterations, [&](std::size_t) {
        auto cloned = sourceSpan.clone();
        observed += cloned.attributes().size() + cloned.events().size() + cloned.links().size();
        do_not_optimize(cloned);
    });

    const auto spanMoveNs = measure_ns_per_op(iterations, [&](std::size_t i) {
        std::vector<galay::tracing::Span> spans;
        spans.reserve(1);
        spans.push_back(make_span("move-span-" + std::to_string(i)));
        observed += spans.front().name().size();
        do_not_optimize(spans);
    });

    const auto sourceRecord = make_log_record();
    const auto logCloneNs = measure_ns_per_op(iterations, [&](std::size_t) {
        auto cloned = sourceRecord.clone();
        observed += cloned.message.size();
        do_not_optimize(cloned);
    });

    const auto logMoveNs = measure_ns_per_op(iterations, [&](std::size_t) {
        std::vector<galay::tracing::LogRecord> records;
        records.reserve(1);
        records.push_back(make_log_record());
        observed += records.front().message.size();
        do_not_optimize(records);
    });

    const auto sourceSnapshot = make_snapshot();
    const auto snapshotCloneNs = measure_ns_per_op(iterations, [&](std::size_t) {
        auto cloned = sourceSnapshot.clone();
        observed += cloned.sinks.size();
        do_not_optimize(cloned);
    });

    const auto snapshotMoveNs = measure_ns_per_op(iterations, [&](std::size_t) {
        std::vector<galay::tracing::Logger::SinkSnapshot> snapshots;
        snapshots.reserve(1);
        snapshots.push_back(make_snapshot());
        observed += snapshots.front().sinks.size();
        do_not_optimize(snapshots);
    });

    const auto exporterConstructNs = measure_ns_per_op(iterations, [&](std::size_t) {
        galay::tracing::OtlpHttpExporter exporter({}, [](galay::tracing::OtlpHttpRequest) {
            return galay::tracing::OtlpHttpResponse{.status_code = 200};
        });
        do_not_optimize(exporter);
    });

    const auto sinkConstructNs = measure_ns_per_op(iterations, [&](std::size_t) {
        galay::tracing::ConsoleSink sink;
        do_not_optimize(sink);
    });

    std::cout << "B10-MoveCloneOwnership workload=" << iterations
              << " build=" << build_type()
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
