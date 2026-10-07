#include <galay/cpp/galay-tracing/kernel/sampler.h>
#include <galay/cpp/galay-tracing/kernel/span_guard.h>
#include <galay/cpp/galay-tracing/kernel/span_processor.h>
#include <galay/cpp/galay-tracing/kernel/tracer_provider.h>

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class RecordingProcessor final : public galay::tracing::SpanProcessor {
public:
    void on_end(galay::tracing::Span&& span) override {
        ++calls;
        if (throw_on_end) {
            throw std::runtime_error("processor failure");
        }
        spans.push_back(std::move(span));
    }

    bool force_flush(std::chrono::milliseconds) override {
        return true;
    }

    bool shutdown(std::chrono::milliseconds) override {
        return true;
    }

    int calls{0};
    bool throw_on_end{false};
    std::vector<galay::tracing::Span> spans;
};

class SamplerScope {
public:
    explicit SamplerScope(const galay::tracing::Sampler* sampler) noexcept {
        galay::tracing::set_sampler(sampler);
    }

    ~SamplerScope() {
        galay::tracing::set_sampler(nullptr);
    }
};

void no_processor_is_configured_by_default() {
    galay::tracing::SpanProcessorScope processor_scope(nullptr);
    assert(galay::tracing::current_span_processor() == nullptr);

    {
        auto guard = galay::tracing::start_span("default-noop");
        assert(guard.span().span_context().sampled());
    }

    assert(!galay::tracing::current_context().has_value());
}

void sampled_guard_destruction_enqueues_once() {
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::start_span("sampled");
        assert(processor.calls == 0);
    }

    assert(processor.calls == 1);
    assert(processor.spans.size() == 1);
    assert(processor.spans[0].name() == "sampled");
    assert(processor.spans[0].ended());
    assert(processor.spans[0].span_context().sampled());
}

void explicit_end_is_idempotent_and_destruction_enqueues_once() {
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::start_span("manual-end");
        guard.end();
        guard.end();
        assert(guard.span().ended());
        assert(processor.calls == 0);
    }

    assert(processor.calls == 1);
    assert(processor.spans.size() == 1);
    assert(processor.spans[0].name() == "manual-end");
}

void moved_guard_enqueues_only_from_active_owner() {
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        galay::tracing::SpanGuard moved;
        {
            auto guard = galay::tracing::start_span("moved");
            moved = std::move(guard);
        }
        assert(processor.calls == 0);
    }

    assert(processor.calls == 1);
    assert(processor.spans.size() == 1);
    assert(processor.spans[0].name() == "moved");
}

void unsampled_spans_do_not_enqueue() {
    galay::tracing::AlwaysOffSampler off;
    SamplerScope sampler_scope(&off);
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::start_span("unsampled");
        assert(!guard.span().span_context().sampled());
    }

    assert(processor.calls == 0);
    assert(processor.spans.empty());
}

void processor_exceptions_do_not_escape_destructors() {
    RecordingProcessor processor;
    processor.throw_on_end = true;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::start_span("throwing-processor");
        assert(guard.span().span_context().sampled());
    }

    assert(processor.calls == 1);
}

} // namespace

int main() {
    no_processor_is_configured_by_default();
    sampled_guard_destruction_enqueues_once();
    explicit_end_is_idempotent_and_destruction_enqueues_once();
    moved_guard_enqueues_only_from_active_owner();
    unsampled_spans_do_not_enqueue();
    processor_exceptions_do_not_escape_destructors();
}
