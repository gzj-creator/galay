#include "galay-tracing/kernel/sampler.h"
#include "galay-tracing/kernel/span_guard.h"
#include "galay-tracing/kernel/span_processor.h"
#include "galay-tracing/kernel/tracer_provider.h"

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class RecordingProcessor final : public galay::tracing::SpanProcessor {
public:
    void onEnd(galay::tracing::Span&& span) override {
        ++calls;
        if (throw_on_end) {
            throw std::runtime_error("processor failure");
        }
        spans.push_back(std::move(span));
    }

    bool forceFlush(std::chrono::milliseconds) override {
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
        galay::tracing::setSampler(sampler);
    }

    ~SamplerScope() {
        galay::tracing::setSampler(nullptr);
    }
};

void noProcessorIsConfiguredByDefault() {
    galay::tracing::SpanProcessorScope processor_scope(nullptr);
    assert(galay::tracing::currentSpanProcessor() == nullptr);

    {
        auto guard = galay::tracing::startSpan("default-noop");
        assert(guard.span().spanContext().sampled());
    }

    assert(!galay::tracing::currentContext().has_value());
}

void sampledGuardDestructionEnqueuesOnce() {
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::startSpan("sampled");
        assert(processor.calls == 0);
    }

    assert(processor.calls == 1);
    assert(processor.spans.size() == 1);
    assert(processor.spans[0].name() == "sampled");
    assert(processor.spans[0].ended());
    assert(processor.spans[0].spanContext().sampled());
}

void explicitEndIsIdempotentAndDestructionEnqueuesOnce() {
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::startSpan("manual-end");
        guard.end();
        guard.end();
        assert(guard.span().ended());
        assert(processor.calls == 0);
    }

    assert(processor.calls == 1);
    assert(processor.spans.size() == 1);
    assert(processor.spans[0].name() == "manual-end");
}

void movedGuardEnqueuesOnlyFromActiveOwner() {
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        galay::tracing::SpanGuard moved;
        {
            auto guard = galay::tracing::startSpan("moved");
            moved = std::move(guard);
        }
        assert(processor.calls == 0);
    }

    assert(processor.calls == 1);
    assert(processor.spans.size() == 1);
    assert(processor.spans[0].name() == "moved");
}

void unsampledSpansDoNotEnqueue() {
    galay::tracing::AlwaysOffSampler off;
    SamplerScope sampler_scope(&off);
    RecordingProcessor processor;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::startSpan("unsampled");
        assert(!guard.span().spanContext().sampled());
    }

    assert(processor.calls == 0);
    assert(processor.spans.empty());
}

void processorExceptionsDoNotEscapeDestructors() {
    RecordingProcessor processor;
    processor.throw_on_end = true;
    galay::tracing::SpanProcessorScope processor_scope(&processor);

    {
        auto guard = galay::tracing::startSpan("throwing-processor");
        assert(guard.span().spanContext().sampled());
    }

    assert(processor.calls == 1);
}

} // namespace

int main() {
    noProcessorIsConfiguredByDefault();
    sampledGuardDestructionEnqueuesOnce();
    explicitEndIsIdempotentAndDestructionEnqueuesOnce();
    movedGuardEnqueuesOnlyFromActiveOwner();
    unsampledSpansDoNotEnqueue();
    processorExceptionsDoNotEscapeDestructors();
}
