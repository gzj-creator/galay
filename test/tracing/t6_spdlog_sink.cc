#include "galay-tracing/adapters/spdlog_sink.h"

#include "galay-tracing/log/log_record.h"

#include <cassert>
#include <memory>
#include <sstream>
#include <string>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

int main() {
    auto stream = std::make_shared<std::ostringstream>();
    auto ostreamSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*stream);
    auto logger = std::make_shared<spdlog::logger>("test", ostreamSink);
    logger->set_pattern("%v");

    galay::tracing::SpdlogSink sink(logger);
    const auto context = galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);

    sink.write(galay::tracing::LogRecord{
        .level = galay::tracing::LogLevel::kInfo,
        .message = "accepted",
        .source = {"test.cc", 9, "main"},
        .context = galay::tracing::makeLogContext(context),
    });
    logger->flush();

    const auto line = stream->str();
    assert(line.find("trace_id=4bf92f3577b34da6a3ce929d0e0e4736") != std::string::npos);
    assert(line.find("span_id=00f067aa0ba902b7") != std::string::npos);
    assert(line.find("accepted") != std::string::npos);
}
