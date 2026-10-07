#include <galay/cpp/galay-tracing/adapters/kernel_context.h>

#include <galay/cpp/galay-tracing/context/context_storage.h>
#include <galay/cpp/galay-tracing/log/logger.h>
#include <galay/cpp/galay-tracing/log/log_sink.h>

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <cassert>
#include <memory>
#include <string_view>
#include <vector>

namespace {

class TestSink final : public galay::tracing::LogSink {
public:
    void write(const galay::tracing::LogRecord& record) override {
        records.push_back(record.clone());
    }

    std::vector<galay::tracing::LogRecord> records;
};

galay::kernel::Task<void> log_inside_kernel_task(std::optional<galay::tracing::TraceContext> context) {
    assert(!galay::tracing::current_context().has_value());
    galay::tracing::log(context).info("inside kernel task");
    co_return;
}

galay::kernel::Task<void> log_without_context_task() {
    assert(!galay::tracing::current_context().has_value());
    GALAY_LOG_INFO("unwrapped task");
    co_return;
}

galay::kernel::Task<void> yielding_explicit_context_task(std::optional<galay::tracing::TraceContext> context) {
    assert(!galay::tracing::current_context().has_value());
    galay::tracing::log(context).info("wrapped before yield");
    auto runtimeHandle = galay::kernel::RuntimeHandle::current();
    assert(runtimeHandle.has_value());
    auto spawned = runtimeHandle->spawn_cpu(log_without_context_task());
    assert(spawned.has_value());
    co_yield true;
    assert(!galay::tracing::current_context().has_value());
    galay::tracing::log(context).info("wrapped after yield");
    co_return;
}

const galay::tracing::LogRecord* find_record(const TestSink& sink, std::string_view message) {
    for (const auto& record : sink.records) {
        if (record.message == message) {
            return &record;
        }
    }
    return nullptr;
}

galay::tracing::TraceContext make_test_context() {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex("00f067aa0ba902b7"),
        0x01);
}

void explicit_context_reaches_kernel_task() {
    auto sink = std::make_shared<TestSink>();
    galay::tracing::Logger logger;
    logger.clear_sinks();
    logger.add_sink(sink);
    galay::tracing::set_default_logger(&logger);

    const auto context = make_test_context();
    galay::tracing::set_current_context(context);
    auto captured = galay::tracing::capture_trace_context();
    auto task = log_inside_kernel_task(captured);
    galay::tracing::clear_current_context();

    auto runtime = galay::kernel::RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .build();
    runtime.block_on_io(std::move(task));

    galay::tracing::set_default_logger(nullptr);

    const auto* record = find_record(*sink, "inside kernel task");
    assert(record != nullptr);
    assert(record->context.has_value());
    assert(record->context->trace_id() == context.trace_id());
    assert(record->context->span_id() == context.span_id());
}

void explicit_context_survives_yield_without_leaking_to_other_tasks() {
    auto sink = std::make_shared<TestSink>();
    galay::tracing::Logger logger;
    logger.clear_sinks();
    logger.add_sink(sink);
    galay::tracing::set_default_logger(&logger);

    const auto context = make_test_context();
    galay::tracing::set_current_context(context);
    auto captured = galay::tracing::capture_trace_context();
    auto wrapped = yielding_explicit_context_task(captured);
    galay::tracing::clear_current_context();

    auto runtime = galay::kernel::RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .build();

    auto wrappedHandle = runtime.spawn_cpu(std::move(wrapped));
    assert(wrappedHandle.has_value());
    auto wrappedWait = wrappedHandle->wait();
    assert(wrappedWait.has_value());
    auto wrappedJoin = wrappedHandle->join();
    assert(wrappedJoin.has_value());

    galay::tracing::set_default_logger(nullptr);

    const auto* wrappedRecord = find_record(*sink, "wrapped before yield");
    const auto* resumedRecord = find_record(*sink, "wrapped after yield");
    const auto* unwrappedRecord = find_record(*sink, "unwrapped task");

    assert(wrappedRecord != nullptr);
    assert(wrappedRecord->context.has_value());
    assert(wrappedRecord->context->trace_id() == context.trace_id());

    assert(resumedRecord != nullptr);
    assert(resumedRecord->context.has_value());
    assert(resumedRecord->context->trace_id() == context.trace_id());

    assert(unwrappedRecord != nullptr);
    assert(!unwrappedRecord->context.has_value());
}

} // namespace

int main() {
    explicit_context_reaches_kernel_task();
    explicit_context_survives_yield_without_leaking_to_other_tasks();
}
