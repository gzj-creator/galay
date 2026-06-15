#include "galay-tracing/log/logger.h"
#include "galay-tracing/log/log_sink.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>

namespace {

template <typename T>
void doNotOptimize(const T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    (void)value;
#endif
}

int blackBoxInt(int value) noexcept {
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

    [[nodiscard]] bool isEnabled(galay::tracing::LogLevel level) const noexcept {
        return minLevel != galay::tracing::LogLevel::kOff &&
            static_cast<int>(level) >= static_cast<int>(minLevel);
    }

    [[gnu::noinline]] void write(galay::tracing::StructuredLogRecord record) noexcept {
        ++count;
        fieldCount += record.fields.size();
        doNotOptimize(record.name);
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

[[nodiscard]] const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

[[gnu::noinline]] galay::tracing::LogLevel disabledThreshold() noexcept {
    return galay::tracing::LogLevel::kError;
}

[[gnu::noinline]] galay::tracing::LogLevel enabledThreshold() noexcept {
    return galay::tracing::LogLevel::kInfo;
}

double measureDisabledNs() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabledThreshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        galay::tracing::event(std::nullopt, writer)
            .debug("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measureEnabledNs(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabledThreshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        galay::tracing::event(std::nullopt, writer)
            .info("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measureDefaultDisabledNs() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabledThreshold());
    galay::tracing::setDefaultLogWriter(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        galay::tracing::event(std::nullopt)
            .debug("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::setDefaultLogWriter(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measureDefaultEnabledNs(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabledThreshold());
    galay::tracing::setDefaultLogWriter(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        galay::tracing::event(std::nullopt)
            .info("value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::setDefaultLogWriter(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measureMacroDisabledNs() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabledThreshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        const int value = blackBoxInt(i);
        GALAY_EVENT_DEBUG(writer, std::nullopt, "value", galay::tracing::field("value", value));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measureMacroEnabledNs(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabledThreshold());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        GALAY_EVENT_INFO(writer, std::nullopt, "value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measureDefaultMacroDisabledNs() {
    constexpr int kIterations = 200000;
    StructuredNoopWriter writer(disabledThreshold());
    galay::tracing::setDefaultLogWriter(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        const int value = blackBoxInt(i);
        GALAY_EVENT_DEBUG_DEFAULT(std::nullopt, "value", galay::tracing::field("value", value));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::setDefaultLogWriter(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    return static_cast<double>(ns) / kIterations;
}

double measureDefaultMacroEnabledNs(std::size_t& writes, std::size_t& fields) {
    constexpr int kIterations = 100000;
    StructuredNoopWriter writer(enabledThreshold());
    galay::tracing::setDefaultLogWriter(&writer);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
        GALAY_EVENT_INFO_DEFAULT(std::nullopt, "value", galay::tracing::field("value", i));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    galay::tracing::setDefaultLogWriter(nullptr);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    writes = writer.count;
    fields = writer.fieldCount;
    return static_cast<double>(ns) / kIterations;
}

double measureLoggerFallbackNs(std::size_t& writes) {
    constexpr int kIterations = 100000;
    auto sink = std::make_shared<NullSink>();
    galay::tracing::Logger logger(galay::tracing::LogLevel::kInfo);
    logger.clearSinks();
    logger.addSink(sink);

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        doNotOptimize(i);
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
    std::size_t writes = 0;
    std::size_t fields = 0;
    const double disabled = measureDisabledNs();
    const double enabled = measureEnabledNs(writes, fields);
    std::size_t defaultWrites = 0;
    std::size_t defaultFields = 0;
    const double defaultDisabled = measureDefaultDisabledNs();
    const double defaultEnabled = measureDefaultEnabledNs(defaultWrites, defaultFields);
    std::size_t macroWrites = 0;
    std::size_t macroFields = 0;
    const double macroDisabled = measureMacroDisabledNs();
    const double macroEnabled = measureMacroEnabledNs(macroWrites, macroFields);
    std::size_t defaultMacroWrites = 0;
    std::size_t defaultMacroFields = 0;
    const double defaultMacroDisabled = measureDefaultMacroDisabledNs();
    const double defaultMacroEnabled = measureDefaultMacroEnabledNs(defaultMacroWrites, defaultMacroFields);
    std::size_t fallbackWrites = 0;
    const double loggerFallback = measureLoggerFallbackNs(fallbackWrites);

    std::cout << "B6-StructuredEvent workload_disabled=200000 workload_enabled=100000 build=" << buildType()
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
