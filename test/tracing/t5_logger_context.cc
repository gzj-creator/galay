#include "galay-tracing/context/context_storage.h"
#include "galay-tracing/log/logger.h"
#include "galay-tracing/log/log_sink.h"

#include <cassert>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

struct ExplodingFormat {};

} // namespace

template <>
struct std::formatter<ExplodingFormat> {
    constexpr auto parse(std::format_parse_context& context) {
        return context.begin();
    }

    auto format(const ExplodingFormat&, std::format_context& context) const {
        throw std::runtime_error("disabled log level formatted an argument");
        return context.out();
    }
};

namespace {

static_assert(std::is_same_v<
    decltype(std::declval<galay::tracing::LogRecord>().context),
    std::optional<galay::tracing::LogContext>>);
static_assert(sizeof(galay::tracing::LogContext) <= 32);
static_assert(sizeof(std::optional<galay::tracing::LogContext>) <= 40);
static_assert(sizeof(galay::tracing::LogContext) < sizeof(galay::tracing::TraceContext));
static_assert(sizeof(galay::tracing::LogFieldValue) <= 32);
static_assert(sizeof(galay::tracing::LogField) <= 48);
static_assert(sizeof(galay::tracing::detail::DefaultLogWriter) <= sizeof(void*));

class TestSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord& record) override {
        records.push_back(record);
    }

    std::vector<galay::tracing::LogRecord> records;
};

class TestWriter {
public:
    explicit TestWriter(galay::tracing::LogLevel level = galay::tracing::LogLevel::kTrace)
        : minLevel(level) {
    }

    bool isEnabled(galay::tracing::LogLevel level) const noexcept {
        return minLevel != galay::tracing::LogLevel::kOff &&
            static_cast<int>(level) >= static_cast<int>(minLevel);
    }

    void write(galay::tracing::LogRecord record) {
        records.push_back(std::move(record));
    }

    galay::tracing::LogLevel minLevel;
    std::vector<galay::tracing::LogRecord> records;
};

class StructuredWriter {
public:
    bool isEnabled(galay::tracing::LogLevel level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(minLevel);
    }

    void write(galay::tracing::StructuredLogRecord record) {
        level = record.level;
        name = std::string(record.name);
        fieldCount = record.fields.size();
        if (!record.fields.empty()) {
            firstFieldName = std::string(record.fields[0].name);
            firstFieldValue = record.fields[0].value.asInt64();
        }
        context = record.context;
    }

    galay::tracing::LogLevel minLevel{galay::tracing::LogLevel::kTrace};
    galay::tracing::LogLevel level{galay::tracing::LogLevel::kOff};
    std::string name;
    std::size_t fieldCount{0};
    std::string firstFieldName;
    std::int64_t firstFieldValue{0};
    std::optional<galay::tracing::LogContext> context;
};

class RefcountSink final : public galay::tracing::LogSink {
public:
    explicit RefcountSink(std::weak_ptr<RefcountSink>* selfRef)
        : m_selfRef(selfRef) {
    }

    void write(const galay::tracing::LogRecord&) override {
        observedUseCount = m_selfRef->use_count();
    }

    long observedUseCount{0};

private:
    std::weak_ptr<RefcountSink>* m_selfRef;
};

galay::tracing::LogField countedField(int& evaluations) {
    ++evaluations;
    return galay::tracing::field("order_id", 42);
}

galay::tracing::TraceContext makeTestContext() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
}

void noContextLogsStillEmit() {
    galay::tracing::clearCurrentContext();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clearSinks();
    logger.addSink(sink);

    logger.log(galay::tracing::LogLevel::kInfo, {"test.cc", 42, "noContextLogsStillEmit"}, "hello {}", "world");

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "hello world");
    assert(!sink->records[0].context.has_value());
    assert(sink->records[0].source.line == 42);
}

void contextLogsIncludeTraceAndSpanIds() {
    auto context = makeTestContext();
    galay::tracing::setCurrentContext(context);

    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clearSinks();
    logger.addSink(sink);

    logger.log(galay::tracing::LogLevel::kInfo, {"test.cc", 7, "contextLogsIncludeTraceAndSpanIds"}, "accepted");

    assert(sink->records.size() == 1);
    assert(sink->records[0].context.has_value());
    assert(sink->records[0].context->traceId() == context.traceId());
    assert(sink->records[0].context->spanId() == context.spanId());

    galay::tracing::clearCurrentContext();
}

void contextProxyLogsThroughDefaultWriter() {
    const auto context = makeTestContext();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clearSinks();
    logger.addSink(sink);
    galay::tracing::setDefaultLogWriter(&logger);

    galay::tracing::log(context).info("proxy {}", 42);

    galay::tracing::setDefaultLogWriter(nullptr);

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "proxy 42");
    assert(sink->records[0].context.has_value());
    assert(sink->records[0].context->traceId() == context.traceId());
    assert(sink->records[0].context->spanId() == context.spanId());
}

void contextProxyCanUseExplicitWriter() {
    const auto context = makeTestContext();
    TestWriter writer;

    galay::tracing::log(context, writer).warn("writer {}", 7);

    assert(writer.records.size() == 1);
    assert(writer.records[0].level == galay::tracing::LogLevel::kWarn);
    assert(writer.records[0].message == "writer 7");
    assert(writer.records[0].context.has_value());
    assert(writer.records[0].context->traceId() == context.traceId());
}

void disabledLevelDoesNotFormat() {
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clearSinks();
    logger.addSink(sink);
    logger.setLevel(galay::tracing::LogLevel::kWarn);

    logger.log(galay::tracing::LogLevel::kDebug, {"test.cc", 11, "disabledLevelDoesNotFormat"}, "{}", ExplodingFormat{});

    assert(sink->records.empty());
}

void disabledProxyWriterDoesNotFormat() {
    TestWriter writer(galay::tracing::LogLevel::kError);

    galay::tracing::log(makeTestContext(), writer).info("{}", ExplodingFormat{});

    assert(writer.records.empty());
}

void macrosCaptureFileAndLine() {
    galay::tracing::clearCurrentContext();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clearSinks();
    logger.addSink(sink);
    galay::tracing::setDefaultLogger(&logger);

    const auto expectedLine = __LINE__ + 1;
    GALAY_LOG_INFO("macro {}", 7);

    galay::tracing::setDefaultLogger(nullptr);

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "macro 7");
    assert(std::string_view(sink->records[0].source.file).ends_with("t5_logger_context.cc"));
    assert(sink->records[0].source.line == expectedLine);
}

void publishDoesNotCopySinkSharedPointers() {
    std::weak_ptr<RefcountSink> weak;
    auto sink = std::make_shared<RefcountSink>(&weak);
    weak = sink;

    galay::tracing::Logger logger;
    logger.clearSinks();
    logger.addSink(sink);

    logger.log(galay::tracing::LogLevel::kInfo, {"test.cc", 88, "publishDoesNotCopySinkSharedPointers"}, "snapshot");

    assert(sink->observedUseCount == 2);
}

void structuredEventUsesExplicitWriter() {
    const auto context = makeTestContext();
    StructuredWriter writer;

    galay::tracing::event(context, writer).info("order_sent", galay::tracing::field("order_id", 42));

    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->traceId() == context.traceId());
}

void structuredEventUsesDefaultStructuredWriter() {
    const auto context = makeTestContext();
    StructuredWriter writer;
    galay::tracing::setDefaultLogWriter(&writer);

    galay::tracing::event(context).info("order_sent", galay::tracing::field("order_id", 42));

    galay::tracing::setDefaultLogWriter(nullptr);

    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->traceId() == context.traceId());
}

void structuredEventCanUseLoggerSink() {
    const auto context = makeTestContext();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clearSinks();
    logger.addSink(sink);

    galay::tracing::event(context, logger).info("order_sent", galay::tracing::field("order_id", 42));

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "order_sent order_id=42");
    assert(sink->records[0].context.has_value());
    assert(sink->records[0].context->traceId() == context.traceId());
}

void eventMacroDoesNotEvaluateDisabledFields() {
    StructuredWriter writer;
    writer.minLevel = galay::tracing::LogLevel::kError;
    int evaluations = 0;

    GALAY_EVENT_DEBUG(writer, makeTestContext(), "order_sent", countedField(evaluations));

    assert(evaluations == 0);
    assert(writer.name.empty());
}

void eventMacroWritesEnabledStructuredEvent() {
    const auto context = makeTestContext();
    StructuredWriter writer;
    int evaluations = 0;

    GALAY_EVENT_INFO(writer, context, "order_sent", countedField(evaluations));

    assert(evaluations == 1);
    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->traceId() == context.traceId());
}

void defaultEventMacroDoesNotEvaluateDisabledFields() {
    StructuredWriter writer;
    writer.minLevel = galay::tracing::LogLevel::kError;
    galay::tracing::setDefaultLogWriter(&writer);
    int evaluations = 0;

    GALAY_EVENT_DEBUG_DEFAULT(makeTestContext(), "order_sent", countedField(evaluations));

    galay::tracing::setDefaultLogWriter(nullptr);

    assert(evaluations == 0);
    assert(writer.name.empty());
}

void defaultEventMacroWritesEnabledStructuredEvent() {
    const auto context = makeTestContext();
    StructuredWriter writer;
    galay::tracing::setDefaultLogWriter(&writer);
    int evaluations = 0;

    GALAY_EVENT_INFO_DEFAULT(context, "order_sent", countedField(evaluations));

    galay::tracing::setDefaultLogWriter(nullptr);

    assert(evaluations == 1);
    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->traceId() == context.traceId());
}

} // namespace

int main() {
    noContextLogsStillEmit();
    contextLogsIncludeTraceAndSpanIds();
    contextProxyLogsThroughDefaultWriter();
    contextProxyCanUseExplicitWriter();
    disabledLevelDoesNotFormat();
    disabledProxyWriterDoesNotFormat();
    macrosCaptureFileAndLine();
    publishDoesNotCopySinkSharedPointers();
    structuredEventUsesExplicitWriter();
    structuredEventUsesDefaultStructuredWriter();
    structuredEventCanUseLoggerSink();
    eventMacroDoesNotEvaluateDisabledFields();
    eventMacroWritesEnabledStructuredEvent();
    defaultEventMacroDoesNotEvaluateDisabledFields();
    defaultEventMacroWritesEnabledStructuredEvent();
}
