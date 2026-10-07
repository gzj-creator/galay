#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-tracing/log/logger.h>
#include <galay/cpp/galay-tracing/log/log_sink.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>

namespace {

template <typename T>
void do_not_optimize(const T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    (void)value;
#endif
}

int black_box_int(int value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : "+r"(value) : : "memory");
#endif
    return value;
}

class StructuredNoopWriter {
public:
    explicit StructuredNoopWriter(galay::tracing::LogLevel level)
        : minLevel(level) {
    }

    [[nodiscard]] bool is_enabled(galay::tracing::LogLevel level) const noexcept {
        return minLevel != galay::tracing::LogLevel::kOff &&
            static_cast<int>(level) >= static_cast<int>(minLevel);
    }

    [[gnu::noinline]] void write(galay::tracing::StructuredLogRecord record) noexcept {
        ++count;
        fieldCount += record.fields.size();
        do_not_optimize(record.name);
    }

    galay::tracing::LogLevel minLevel;
    std::size_t count{0};
    std::size_t fieldCount{0};
};

class NullSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord&) override {
        ++count;
    }

    std::size_t count{0};
};

[[nodiscard]] const char* build_type() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

[[gnu::noinline]] galay::tracing::LogLevel disabled_threshold() noexcept {
    return galay::tracing::LogLevel::kError;
}

[[gnu::noinline]] galay::tracing::LogLevel enabled_threshold() noexcept {
    return galay::tracing::LogLevel::kInfo;
}

double measure_disabled_ns() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabled_threshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        galay::tracing::event(std::nullopt, writer)
            .debug("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measure_enabled_ns(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabled_threshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        galay::tracing::event(std::nullopt, writer)
            .info("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measure_default_disabled_ns() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabled_threshold());
    galay::tracing::set_default_log_writer(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        galay::tracing::event(std::nullopt)
            .debug("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::set_default_log_writer(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measure_default_enabled_ns(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabled_threshold());
    galay::tracing::set_default_log_writer(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        galay::tracing::event(std::nullopt)
            .info("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::set_default_log_writer(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measure_macro_disabled_ns() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabled_threshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        const int value = black_box_int(i);
        GALAY_EVENT_DEBUG(writer, std::nullopt, "value", galay::tracing::field("value", value));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measure_macro_enabled_ns(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabled_threshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        GALAY_EVENT_INFO(writer, std::nullopt, "value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measure_default_macro_disabled_ns() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabled_threshold());
    galay::tracing::set_default_log_writer(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        const int value = black_box_int(i);
        GALAY_EVENT_DEBUG_DEFAULT(std::nullopt, "value", galay::tracing::field("value", value));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::set_default_log_writer(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measure_default_macro_enabled_ns(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabled_threshold());
    galay::tracing::set_default_log_writer(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        GALAY_EVENT_INFO_DEFAULT(std::nullopt, "value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::set_default_log_writer(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measure_logger_fallback_ns(std::size_t& writes) {
    constexpr int kIterations = 100000;
    auto sink = std::make_shared<NullSink>();
    galay::tracing::Logger logger(galay::tracing::LogLevel::kInfo);
    logger.clear_sinks();
    logger.add_sink(sink);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        do_not_optimize(i);
        galay::tracing::event(std::nullopt, logger)
            .info("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = sink->count;
    return static_cast<double>(ns) / kIterations;
}

} // namespace

int main() {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    std::size_t writes = 0;
    std::size_t fields = 0;
    const double disabled = measure_disabled_ns();
    const double enabled = measure_enabled_ns(writes, fields);
    std::size_t defaultWrites = 0;
    std::size_t defaultFields = 0;
    const double defaultDisabled = measure_default_disabled_ns();
    const double defaultEnabled = measure_default_enabled_ns(defaultWrites, defaultFields);
    std::size_t macroWrites = 0;
    std::size_t macroFields = 0;
    const double macroDisabled = measure_macro_disabled_ns();
    const double macroEnabled = measure_macro_enabled_ns(macroWrites, macroFields);
    std::size_t defaultMacroWrites = 0;
    std::size_t defaultMacroFields = 0;
    const double defaultMacroDisabled = measure_default_macro_disabled_ns();
    const double defaultMacroEnabled = measure_default_macro_enabled_ns(defaultMacroWrites, defaultMacroFields);
    std::size_t fallbackWrites = 0;
    const double loggerFallback = measure_logger_fallback_ns(fallbackWrites);

    std::cout << "B6-StructuredEvent workload_disabled=200000 workload_enabled=100000 build=" << build_type()
              << " backend=structured_noop"
              << " explicit_disabled_ns_per_event=" << disabled
              << " explicit_enabled_ns_per_event=" << enabled
              << " writes=" << writes
              << " fields=" << fields
              << " default_disabled_ns_per_event=" << defaultDisabled
              << " default_enabled_ns_per_event=" << defaultEnabled
              << " default_writes=" << defaultWrites
              << " default_fields=" << defaultFields
              << " macro_disabled_ns_per_event=" << macroDisabled
              << " macro_enabled_ns_per_event=" << macroEnabled
              << " macro_writes=" << macroWrites
              << " macro_fields=" << macroFields
              << " default_macro_disabled_ns_per_event=" << defaultMacroDisabled
              << " default_macro_enabled_ns_per_event=" << defaultMacroEnabled
              << " default_macro_writes=" << defaultMacroWrites
              << " default_macro_fields=" << defaultMacroFields
              << " logger_fallback_ns_per_event=" << loggerFallback
              << " fallback_writes=" << fallbackWrites << '\n';
}
