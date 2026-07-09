#include <galay/cpp/galay-tracing/kernel/batch_span_processor.h>
#include <galay/cpp/galay-tracing/kernel/file_span_exporter.h>
#include <galay/cpp/galay-tracing/kernel/otlp_http_exporter.h>
#include <galay/cpp/galay-tracing/log/console_sink.h>
#include <galay/cpp/galay-tracing/log/logger.h>

#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace {

template <typename T>
concept ValueClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template <typename T>
constexpr bool kCopyable =
    std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>;

template <typename T>
constexpr bool kMovable =
    std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>;

static_assert(!kCopyable<galay::tracing::Span>);
static_assert(kMovable<galay::tracing::Span>);
static_assert(ValueClone<galay::tracing::Span>);

static_assert(!kCopyable<galay::tracing::LogRecord>);
static_assert(kMovable<galay::tracing::LogRecord>);
static_assert(ValueClone<galay::tracing::LogRecord>);

static_assert(!kCopyable<galay::tracing::Logger::SinkSnapshot>);
static_assert(kMovable<galay::tracing::Logger::SinkSnapshot>);
static_assert(ValueClone<galay::tracing::Logger::SinkSnapshot>);

static_assert(!kCopyable<galay::tracing::OtlpHttpExporter>);
static_assert(kMovable<galay::tracing::OtlpHttpExporter>);
static_assert(!ValueClone<galay::tracing::OtlpHttpExporter>);

static_assert(!kCopyable<galay::tracing::FileSpanExporter>);
static_assert(!std::is_move_constructible_v<galay::tracing::FileSpanExporter>);
static_assert(!ValueClone<galay::tracing::FileSpanExporter>);

static_assert(!kCopyable<galay::tracing::Logger>);
static_assert(!std::is_move_constructible_v<galay::tracing::Logger>);
static_assert(!ValueClone<galay::tracing::Logger>);

static_assert(!kCopyable<galay::tracing::ConsoleSink>);
static_assert(kMovable<galay::tracing::ConsoleSink>);
static_assert(!ValueClone<galay::tracing::ConsoleSink>);

static_assert(kCopyable<galay::tracing::TraceId>);
static_assert(kCopyable<galay::tracing::SpanId>);
static_assert(kCopyable<galay::tracing::TraceContext>);
static_assert(kCopyable<galay::tracing::SpanContext>);
static_assert(kCopyable<galay::tracing::LogContext>);
static_assert(kCopyable<galay::tracing::StructuredLogRecord>);
static_assert(kCopyable<galay::tracing::BatchSpanProcessorConfig>);
static_assert(kCopyable<galay::tracing::OtlpHttpHeader>);
static_assert(kCopyable<galay::tracing::InstrumentationScopeConfig>);
static_assert(kCopyable<galay::tracing::OtlpHttpExporterConfig>);
static_assert(kCopyable<galay::tracing::OtlpHttpRequest>);
static_assert(kCopyable<galay::tracing::OtlpHttpResponse>);
static_assert(kCopyable<galay::tracing::GalayHttpOtlpTransportConfig>);

class NoopSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord&) override {
    }
};

galay::tracing::TraceContext makeContext(std::string spanId = "00f067aa0ba902b7") {
    auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex(spanId),
        0x01,
        "vendor=value");
    context.setParentSpanId(galay::tracing::SpanId::fromHex("1111111111111111"));
    return context;
}

void spanCloneDuplicatesOwnedState() {
    galay::tracing::Span span("operation", makeContext());
    span.setKind(galay::tracing::SpanKind::kClient);
    span.setStatus(galay::tracing::SpanStatusCode::kError, "timeout");
    assert(span.setAttribute("http.method", "GET"));
    assert(span.addEvent("retry", {galay::tracing::spanAttribute("attempt", 1)}));
    assert(span.addLink(
        galay::tracing::SpanContext(makeContext("2222222222222222")),
        "linked=1",
        {galay::tracing::spanAttribute("link.kind", "batch")}));
    span.end();

    auto cloned = span.clone();
    assert(cloned.name() == span.name());
    assert(cloned.tracestate() == span.tracestate());
    assert(cloned.kind() == span.kind());
    assert(cloned.status().message == "timeout");
    assert(cloned.attributes().size() == 1);
    assert(cloned.events().size() == 1);
    assert(cloned.links().size() == 1);
    assert(cloned.ended());

    cloned.setStatus(galay::tracing::SpanStatusCode::kOk, "ok");
    assert(cloned.setAttribute("http.status_code", 200));
    assert(cloned.addEvent("done"));
    assert(cloned.addLink(galay::tracing::SpanContext(makeContext("3333333333333333"))));

    assert(span.status().message == "timeout");
    assert(span.attributes().size() == 1);
    assert(span.events().size() == 1);
    assert(span.links().size() == 1);
}

void logRecordCloneDuplicatesOwnedState() {
    auto context = makeContext();
    galay::tracing::LogRecord record(
        galay::tracing::LogLevel::kWarn,
        "original message",
        {"test.cc", 17, "logRecordCloneDuplicatesOwnedState"},
        galay::tracing::makeLogContext(context),
        std::chrono::system_clock::time_point(std::chrono::seconds(42)));

    auto cloned = record.clone();
    cloned.message = "changed message";
    cloned.context.reset();
    cloned.source.line = 99;
    cloned.timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(84));

    assert(record.message == "original message");
    assert(record.context.has_value());
    assert(record.source.line == 17);
    assert(record.timestamp == std::chrono::system_clock::time_point(std::chrono::seconds(42)));
}

void sinkSnapshotCloneDuplicatesContainerState() {
    auto first = std::make_shared<NoopSink>();
    auto second = std::make_shared<NoopSink>();

    galay::tracing::Logger::SinkSnapshot snapshot;
    snapshot.sinks.push_back(first);

    auto cloned = snapshot.clone();
    cloned.sinks.push_back(second);
    cloned.sinks.clear();

    assert(snapshot.sinks.size() == 1);
    assert(snapshot.sinks.front() == first);
    assert(first.use_count() == 2);
    assert(second.use_count() == 1);
}

} // namespace

int main() {
    spanCloneDuplicatesOwnedState();
    logRecordCloneDuplicatesOwnedState();
    sinkSnapshotCloneDuplicatesContainerState();
}
