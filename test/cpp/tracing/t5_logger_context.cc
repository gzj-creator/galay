#include <galay/cpp/galay-tracing/context/context_storage.h>
#include <galay/cpp/galay-tracing/log/logger.h>
#include <galay/cpp/galay-tracing/log/log_sink.h>

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
        records.push_back(record.clone());
    }

    std::vector<galay::tracing::LogRecord> records;
};

class TestWriter {
public:
    explicit TestWriter(galay::tracing::LogLevel level = galay::tracing::LogLevel::kTrace)
        : minLevel(level) {
    }

    bool is_enabled(galay::tracing::LogLevel level) const noexcept {
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
    bool is_enabled(galay::tracing::LogLevel level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(minLevel);
    }

    void write(galay::tracing::StructuredLogRecord record) {
        level = record.level;
        name = std::string(record.name);
        fieldCount = record.fields.size();
        if (!record.fields.empty()) {
            firstFieldName = std::string(record.fields[0].name);
            firstFieldValue = record.fields[0].value.as_int64();
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

galay::tracing::LogField counted_field(int& evaluations) {
    ++evaluations;
    return galay::tracing::field("order_id", 42);
}

galay::tracing::TraceContext make_test_context() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
}

void no_context_logs_still_emit() {
    galay::tracing::clear_current_context();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clear_sinks();
    logger.add_sink(sink);

    logger.log(galay::tracing::LogLevel::kInfo, {"test.cc", 42, "noContextLogsStillEmit"}, "hello {}", "world");

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "hello world");
    assert(!sink->records[0].context.has_value());
    assert(sink->records[0].source.line == 42);
}

void context_logs_include_trace_and_span_ids() {
    auto context = make_test_context();
    galay::tracing::set_current_context(context);

    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clear_sinks();
    logger.add_sink(sink);

    logger.log(galay::tracing::LogLevel::kInfo, {"test.cc", 7, "contextLogsIncludeTraceAndSpanIds"}, "accepted");

    assert(sink->records.size() == 1);
    assert(sink->records[0].context.has_value());
    assert(sink->records[0].context->trace_id() == context.trace_id());
    assert(sink->records[0].context->span_id() == context.span_id());

    galay::tracing::clear_current_context();
}

void context_proxy_logs_through_default_writer() {
    const auto context = make_test_context();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clear_sinks();
    logger.add_sink(sink);
    galay::tracing::set_default_log_writer(&logger);

    galay::tracing::log(context).info("proxy {}", 42);

    galay::tracing::set_default_log_writer(nullptr);

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "proxy 42");
    assert(sink->records[0].context.has_value());
    assert(sink->records[0].context->trace_id() == context.trace_id());
    assert(sink->records[0].context->span_id() == context.span_id());
}

void context_proxy_can_use_explicit_writer() {
    const auto context = make_test_context();
    TestWriter writer;

    galay::tracing::log(context, writer).warn("writer {}", 7);

    assert(writer.records.size() == 1);
    assert(writer.records[0].level == galay::tracing::LogLevel::kWarn);
    assert(writer.records[0].message == "writer 7");
    assert(writer.records[0].context.has_value());
    assert(writer.records[0].context->trace_id() == context.trace_id());
}

void disabled_level_does_not_format() {
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clear_sinks();
    logger.add_sink(sink);
    logger.set_level(galay::tracing::LogLevel::kWarn);

    logger.log(galay::tracing::LogLevel::kDebug, {"test.cc", 11, "disabledLevelDoesNotFormat"}, "{}", ExplodingFormat{});

    assert(sink->records.empty());
}

void disabled_proxy_writer_does_not_format() {
    TestWriter writer(galay::tracing::LogLevel::kError);

    galay::tracing::log(make_test_context(), writer).info("{}", ExplodingFormat{});

    assert(writer.records.empty());
}

void macros_capture_file_and_line() {
    galay::tracing::clear_current_context();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clear_sinks();
    logger.add_sink(sink);
    galay::tracing::set_default_logger(&logger);

    const auto expectedLine = __LINE__ + 1;
    GALAY_LOG_INFO("macro {}", 7);

    galay::tracing::set_default_logger(nullptr);

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "macro 7");
    assert(std::string_view(sink->records[0].source.file).ends_with("t5_logger_context.cc"));
    assert(sink->records[0].source.line == expectedLine);
}

void publish_does_not_copy_sink_shared_pointers() {
    std::weak_ptr<RefcountSink> weak;
    auto sink = std::make_shared<RefcountSink>(&weak);
    weak = sink;

    galay::tracing::Logger logger;
    logger.clear_sinks();
    logger.add_sink(sink);

    logger.log(galay::tracing::LogLevel::kInfo, {"test.cc", 88, "publishDoesNotCopySinkSharedPointers"}, "snapshot");

    assert(sink->observedUseCount == 2);
}

void structured_event_uses_explicit_writer() {
    const auto context = make_test_context();
    StructuredWriter writer;

    galay::tracing::event(context, writer).info("order_sent", galay::tracing::field("order_id", 42));

    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->trace_id() == context.trace_id());
}

void structured_event_uses_default_structured_writer() {
    const auto context = make_test_context();
    StructuredWriter writer;
    galay::tracing::set_default_log_writer(&writer);

    galay::tracing::event(context).info("order_sent", galay::tracing::field("order_id", 42));

    galay::tracing::set_default_log_writer(nullptr);

    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->trace_id() == context.trace_id());
}

void structured_event_can_use_logger_sink() {
    const auto context = make_test_context();
    galay::tracing::Logger logger;
    auto sink = std::make_shared<TestSink>();
    logger.clear_sinks();
    logger.add_sink(sink);

    galay::tracing::event(context, logger).info("order_sent", galay::tracing::field("order_id", 42));

    assert(sink->records.size() == 1);
    assert(sink->records[0].message == "order_sent order_id=42");
    assert(sink->records[0].context.has_value());
    assert(sink->records[0].context->trace_id() == context.trace_id());
}

void event_macro_does_not_evaluate_disabled_fields() {
    StructuredWriter writer;
    writer.minLevel = galay::tracing::LogLevel::kError;
    int evaluations = 0;

    GALAY_EVENT_DEBUG(writer, make_test_context(), "order_sent", counted_field(evaluations));

    assert(evaluations == 0);
    assert(writer.name.empty());
}

void event_macro_writes_enabled_structured_event() {
    const auto context = make_test_context();
    StructuredWriter writer;
    int evaluations = 0;

    GALAY_EVENT_INFO(writer, context, "order_sent", counted_field(evaluations));

    assert(evaluations == 1);
    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->trace_id() == context.trace_id());
}

void default_event_macro_does_not_evaluate_disabled_fields() {
    StructuredWriter writer;
    writer.minLevel = galay::tracing::LogLevel::kError;
    galay::tracing::set_default_log_writer(&writer);
    int evaluations = 0;

    GALAY_EVENT_DEBUG_DEFAULT(make_test_context(), "order_sent", counted_field(evaluations));

    galay::tracing::set_default_log_writer(nullptr);

    assert(evaluations == 0);
    assert(writer.name.empty());
}

void default_event_macro_writes_enabled_structured_event() {
    const auto context = make_test_context();
    StructuredWriter writer;
    galay::tracing::set_default_log_writer(&writer);
    int evaluations = 0;

    GALAY_EVENT_INFO_DEFAULT(context, "order_sent", counted_field(evaluations));

    galay::tracing::set_default_log_writer(nullptr);

    assert(evaluations == 1);
    assert(writer.level == galay::tracing::LogLevel::kInfo);
    assert(writer.name == "order_sent");
    assert(writer.fieldCount == 1);
    assert(writer.firstFieldName == "order_id");
    assert(writer.firstFieldValue == 42);
    assert(writer.context.has_value());
    assert(writer.context->trace_id() == context.trace_id());
}

} // namespace

int main() {
    no_context_logs_still_emit();
    context_logs_include_trace_and_span_ids();
    context_proxy_logs_through_default_writer();
    context_proxy_can_use_explicit_writer();
    disabled_level_does_not_format();
    disabled_proxy_writer_does_not_format();
    macros_capture_file_and_line();
    publish_does_not_copy_sink_shared_pointers();
    structured_event_uses_explicit_writer();
    structured_event_uses_default_structured_writer();
    structured_event_can_use_logger_sink();
    event_macro_does_not_evaluate_disabled_fields();
    event_macro_writes_enabled_structured_event();
    default_event_macro_does_not_evaluate_disabled_fields();
    default_event_macro_writes_enabled_structured_event();
}
