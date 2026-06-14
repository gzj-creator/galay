#include "tracing/adapters/kernel_context.h"

#include "tracing/context/context_storage.h"
#include "tracing/log/logger.h"
#include "tracing/log/log_sink.h"

#include "kernel/kernel/runtime.h"
#include "kernel/kernel/task.h"

#include <cassert>
#include <memory>
#include <string_view>
#include <vector>

namespace {

class TestSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord& record) override {
        records.push_back(record);
    }

    std::vector<galay::tracing::LogRecord> records;
};

galay::kernel::Task<void> logInsideKernelTask(std::optional<galay::tracing::TraceContext> context) {
    assert(!galay::tracing::currentContext().has_value());
    galay::tracing::log(context).info("inside kernel task");
    co_return;
}

galay::kernel::Task<void> logWithoutContextTask() {
    assert(!galay::tracing::currentContext().has_value());
    GALAY_LOG_INFO("unwrapped task");
    co_return;
}

galay::kernel::Task<void> yieldingExplicitContextTask(std::optional<galay::tracing::TraceContext> context) {
    assert(!galay::tracing::currentContext().has_value());
    galay::tracing::log(context).info("wrapped before yield");
    galay::kernel::RuntimeHandle::current().spawn(logWithoutContextTask());
    co_yield true;
    assert(!galay::tracing::currentContext().has_value());
    galay::tracing::log(context).info("wrapped after yield");
    co_return;
}

const galay::tracing::LogRecord* findRecord(const TestSink& sink, std::string_view message) {
    for (const auto& record : sink.records) {
        if (record.message == message) {
            return &record;
        }
    }
    return nullptr;
}

galay::tracing::TraceContext makeTestContext() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex("00f067aa0ba902b7"),
        0x01);
}

void explicitContextReachesKernelTask() {
    auto sink = std::make_shared<TestSink>();
    galay::tracing::Logger logger;
    logger.clearSinks();
    logger.addSink(sink);
    galay::tracing::setDefaultLogger(&logger);

    const auto context = makeTestContext();
    galay::tracing::setCurrentContext(context);
    auto captured = galay::tracing::captureTraceContext();
    auto task = logInsideKernelTask(captured);
    galay::tracing::clearCurrentContext();

    auto runtime = galay::kernel::RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();
    runtime.blockOn(std::move(task));

    galay::tracing::setDefaultLogger(nullptr);

    const auto* record = findRecord(*sink, "inside kernel task");
    assert(record != nullptr);
    assert(record->context.has_value());
    assert(record->context->traceId() == context.traceId());
    assert(record->context->spanId() == context.spanId());
}

void explicitContextSurvivesYieldWithoutLeakingToOtherTasks() {
    auto sink = std::make_shared<TestSink>();
    galay::tracing::Logger logger;
    logger.clearSinks();
    logger.addSink(sink);
    galay::tracing::setDefaultLogger(&logger);

    const auto context = makeTestContext();
    galay::tracing::setCurrentContext(context);
    auto captured = galay::tracing::captureTraceContext();
    auto wrapped = yieldingExplicitContextTask(captured);
    galay::tracing::clearCurrentContext();

    auto runtime = galay::kernel::RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    auto wrappedHandle = runtime.spawn(std::move(wrapped));
    wrappedHandle.join();

    galay::tracing::setDefaultLogger(nullptr);

    const auto* wrappedRecord = findRecord(*sink, "wrapped before yield");
    const auto* resumedRecord = findRecord(*sink, "wrapped after yield");
    const auto* unwrappedRecord = findRecord(*sink, "unwrapped task");

    assert(wrappedRecord != nullptr);
    assert(wrappedRecord->context.has_value());
    assert(wrappedRecord->context->traceId() == context.traceId());

    assert(resumedRecord != nullptr);
    assert(resumedRecord->context.has_value());
    assert(resumedRecord->context->traceId() == context.traceId());

    assert(unwrappedRecord != nullptr);
    assert(!unwrappedRecord->context.has_value());
}

} // namespace

int main() {
    explicitContextReachesKernelTask();
    explicitContextSurvivesYieldWithoutLeakingToOtherTasks();
}
