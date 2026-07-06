/**
 * @file batch_span_processor.cc
 * @brief 批量 Span 处理器实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 基于 moodycamel ConcurrentQueue 的无锁队列和后台工作线程实现批量 Span 导出，
 * 支持定时刷新、队列溢出丢弃统计、优雅关闭和并发 forceFlush。
 */

#include <galay/cpp/galay-tracing/kernel/batch_span_processor.h>

#include <galay/cpp/galay-tracing/common/tracing_log.h>

#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <semaphore>
#include <thread>
#include <utility>

namespace galay::tracing {

namespace {

struct SpanQueueTraits : moodycamel::ConcurrentQueueDefaultTraits {
    static constexpr size_t BLOCK_SIZE = 512;
    static constexpr size_t IMPLICIT_INITIAL_INDEX_SIZE = 256;
    static constexpr size_t EXPLICIT_INITIAL_INDEX_SIZE = 256;
};

[[nodiscard]] BatchSpanProcessorConfig normalizeConfig(BatchSpanProcessorConfig config) noexcept {
    config.queue_capacity = std::max<std::size_t>(config.queue_capacity, 1);
    config.max_batch_size = std::clamp<std::size_t>(config.max_batch_size, 1, config.queue_capacity);
    if (config.flush_interval <= std::chrono::milliseconds::zero()) {
        config.flush_interval = std::chrono::milliseconds(1);
    }
    return config;
}

} // namespace

bool SpanExporter::forceFlush(std::chrono::milliseconds) {
    return true;
}

bool SpanExporter::shutdown(std::chrono::milliseconds) {
    return true;
}

struct BatchSpanProcessor::SpanQueue {
    enum class PushResult {
        kAccepted,
        kFull,
        kClosed,
    };

    explicit SpanQueue(std::size_t capacity)
        : spans(capacity),
          capacity(capacity) {
    }

    [[nodiscard]] PushResult tryPush(
        Span&& span,
        const std::atomic<bool>& shutdownRequested,
        std::size_t& queuedCount) {
        const auto previous = size.fetch_add(1, std::memory_order_relaxed);
        if (previous >= capacity) {
            size.fetch_sub(1, std::memory_order_relaxed);
            return PushResult::kFull;
        }

        if (shutdownRequested.load(std::memory_order_acquire)) {
            size.fetch_sub(1, std::memory_order_relaxed);
            return PushResult::kClosed;
        }

        if (spans.enqueue(std::move(span))) {
            queuedCount = previous + 1;
            return PushResult::kAccepted;
        }
        size.fetch_sub(1, std::memory_order_relaxed);
        return PushResult::kFull;
    }

    [[nodiscard]] std::size_t tryPopBulk(std::vector<Span>& batch, std::size_t maxCount) {
        if (maxCount == 0 || size.load(std::memory_order_relaxed) == 0) {
            return 0;
        }

        const auto count = spans.try_dequeue_bulk(consumer, batch.begin(), maxCount);
        if (count != 0) {
            size.fetch_sub(count, std::memory_order_relaxed);
        }
        return count;
    }

    [[nodiscard]] std::size_t queuedCount() const noexcept {
        return size.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool empty() const noexcept {
        return queuedCount() == 0;
    }

    moodycamel::ConcurrentQueue<Span, SpanQueueTraits> spans;
    moodycamel::ConsumerToken consumer{spans};
    std::size_t capacity;
    std::atomic<std::size_t> size{0};
};

struct BatchSpanProcessor::WorkerControl {
    using Clock = std::chrono::steady_clock;

    struct FlushRequest {
        explicit FlushRequest(std::chrono::milliseconds timeout)
            : timeout(timeout),
              deadline(std::chrono::steady_clock::now() + timeout) {
        }

        std::chrono::milliseconds timeout;
        std::chrono::steady_clock::time_point deadline;
        std::binary_semaphore done{0};
        std::atomic<bool> ok{false};
    };

    std::atomic<std::size_t> activeFlushCallers{0};
    std::atomic<Clock::rep> shutdownDeadlineTicks{
        Clock::time_point::max().time_since_epoch().count()};
    std::counting_semaphore<> signal{0};
    moodycamel::ConcurrentQueue<std::shared_ptr<FlushRequest>> flushRequests;
    std::atomic<bool> shutdownRequested{false};
    std::atomic<bool> workerStopped{false};
    std::atomic<bool> joinClaimed{false};
    std::atomic<bool> exporterShutdown{false};
    std::atomic<bool> shutdownDrainOk{true};
    std::atomic<bool> signalPending{false};

    void setShutdownDeadline(Clock::time_point deadline) noexcept {
        shutdownDeadlineTicks.store(deadline.time_since_epoch().count(), std::memory_order_release);
    }

    [[nodiscard]] Clock::time_point shutdownDeadline() const noexcept {
        return Clock::time_point(Clock::duration(
            shutdownDeadlineTicks.load(std::memory_order_acquire)));
    }

    void notify() noexcept {
        if (!signalPending.exchange(true, std::memory_order_acq_rel)) {
            signal.release();
        }
    }

    void forceNotify() noexcept {
        signalPending.store(true, std::memory_order_release);
        signal.release();
    }

    template <class Rep, class Period>
    [[nodiscard]] bool waitFor(const std::chrono::duration<Rep, Period>& timeout) {
        const bool acquired = signal.try_acquire_for(timeout);
        if (acquired) {
            signalPending.store(false, std::memory_order_release);
        }
        return acquired;
    }
};

BatchSpanProcessor::BatchSpanProcessor(std::unique_ptr<SpanExporter> exporter, BatchSpanProcessorConfig config)
    : m_exporter(std::move(exporter)),
      m_config(normalizeConfig(config)),
      m_queue(std::make_unique<SpanQueue>(m_config.queue_capacity)),
      m_control(std::make_unique<WorkerControl>()),
      m_worker(&BatchSpanProcessor::workerLoop, this) {
    TRACING_LOG_INFO("[batch_processor]", "created queue_capacity={} max_batch_size={} flush_interval_ms={}",
                     m_config.queue_capacity,
                     m_config.max_batch_size,
                     m_config.flush_interval.count());
}

BatchSpanProcessor::~BatchSpanProcessor() noexcept {
    static_cast<void>(shutdown(std::chrono::milliseconds(500)));
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void BatchSpanProcessor::onEnd(Span&& span) {
    if (!span.spanContext().sampled()) {
        return;
    }

    bool shouldNotify = false;
    std::size_t queuedCount = 0;
    const auto pushResult = m_queue->tryPush(std::move(span), m_control->shutdownRequested, queuedCount);
    if (pushResult == SpanQueue::PushResult::kFull) {
        m_droppedSpans.fetch_add(1, std::memory_order_relaxed);
        TRACING_LOG_WARN("[batch_processor]", "drop sampled span because queue cannot accept more items capacity={}",
                         m_config.queue_capacity);
    } else if (pushResult == SpanQueue::PushResult::kClosed) {
        m_control->notify();
        return;
    } else {
        switch (m_config.schedule_mode) {
        case BatchSpanScheduleMode::kOnEnd:
            shouldNotify = true;
            break;
        case BatchSpanScheduleMode::kBatchSize:
            shouldNotify = queuedCount >= m_config.max_batch_size && queuedCount - 1 < m_config.max_batch_size;
            break;
        case BatchSpanScheduleMode::kTimed:
            shouldNotify = false;
            break;
        }
    }

    if (shouldNotify) {
        m_control->notify();
    }
}

bool BatchSpanProcessor::forceFlush(std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds::zero()) {
        return false;
    }

    m_control->activeFlushCallers.fetch_add(1, std::memory_order_acq_rel);
    if (m_control->shutdownRequested.load(std::memory_order_acquire) ||
        m_control->workerStopped.load(std::memory_order_acquire)) {
        if (m_control->activeFlushCallers.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
            m_control->shutdownRequested.load(std::memory_order_acquire)) {
            m_control->forceNotify();
        }
        return m_queue->empty();
    }

    auto request = std::make_shared<WorkerControl::FlushRequest>(timeout);
    if (!m_control->flushRequests.enqueue(request)) {
        if (m_control->activeFlushCallers.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
            m_control->shutdownRequested.load(std::memory_order_acquire)) {
            m_control->forceNotify();
        }
        TRACING_LOG_WARN("[batch_processor]", "forceFlush request enqueue failed timeout_ms={}", timeout.count());
        return false;
    }

    if (m_control->activeFlushCallers.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        m_control->shutdownRequested.load(std::memory_order_acquire)) {
        m_control->forceNotify();
    }
    m_control->forceNotify();
    const bool completed = request->done.try_acquire_until(request->deadline);
    const bool ok = completed && request->ok.load(std::memory_order_acquire);
    if (!ok) {
        TRACING_LOG_WARN("[batch_processor]", "forceFlush finished with failure timeout_ms={}", timeout.count());
    }
    return ok;
}

bool BatchSpanProcessor::shutdown(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    m_control->setShutdownDeadline(deadline);
    m_control->shutdownRequested.store(true, std::memory_order_release);
    m_control->forceNotify();

    bool ok = true;

    while (!m_control->workerStopped.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() <= deadline) {
        std::this_thread::yield();
    }

    const bool workerStopped = m_control->workerStopped.load(std::memory_order_acquire);
    ok = ok && workerStopped;
    if (workerStopped &&
        !m_control->joinClaimed.exchange(true, std::memory_order_acq_rel) &&
        m_worker.joinable()) {
        m_worker.join();
    }

    ok = m_control->shutdownDrainOk.load(std::memory_order_acquire) && ok;

    if (workerStopped && m_exporter && !m_control->exporterShutdown.exchange(true, std::memory_order_acq_rel)) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now >= deadline
            ? std::chrono::milliseconds::zero()
            : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        ok = m_exporter->shutdown(remaining) && ok;
    }

    if (!ok) {
        TRACING_LOG_WARN("[batch_processor]", "shutdown finished with failure timeout_ms={}", timeout.count());
    } else if (TRACING_LOG_ENABLED(::galay::kernel::LogLevel::kDebug)) {
        TRACING_LOG_DEBUG("[batch_processor]", "shutdown finished dropped_spans={}", droppedSpanCount());
    }
    return ok && std::chrono::steady_clock::now() <= deadline;
}

std::size_t BatchSpanProcessor::droppedSpanCount() const noexcept {
    return m_droppedSpans.load(std::memory_order_relaxed);
}

void BatchSpanProcessor::workerLoop() {
    std::vector<Span> batch(m_config.max_batch_size);

    auto drainReadyBatches = [this, &batch](bool drainPartial) {
        bool ok = true;
        while (!m_control->shutdownRequested.load(std::memory_order_acquire) &&
               (m_queue->queuedCount() >= m_config.max_batch_size ||
                (drainPartial && !m_queue->empty()))) {
            const auto count = drainQueue(batch, m_config.max_batch_size);
            if (count == 0) {
                break;
            }
            ok = exportBatch(std::span<const Span>(batch.data(), count)) && ok;
        }
        return ok;
    };

    auto remainingTimeout = [](std::chrono::steady_clock::time_point deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    };

    auto drainUntilIdle = [this, &batch](std::chrono::steady_clock::time_point deadline) {
        bool ok = true;
        while (std::chrono::steady_clock::now() <= deadline) {
            bool exportedAny = false;
            while (true) {
                const auto count = drainQueue(batch, m_config.max_batch_size);
                if (count == 0) {
                    break;
                }
                exportedAny = true;
                ok = exportBatch(std::span<const Span>(batch.data(), count)) && ok;
            }
            if (m_queue->empty()) {
                break;
            }
            if (!exportedAny) {
                static_cast<void>(m_control->waitFor(std::chrono::milliseconds(1)));
            }
        }
        return ok && std::chrono::steady_clock::now() <= deadline;
    };

    auto completeFlushRequest = [this, &drainUntilIdle, &remainingTimeout](
                                    const std::shared_ptr<WorkerControl::FlushRequest>& request) {
        bool ok = drainUntilIdle(request->deadline);
        if (m_exporter) {
            ok = m_exporter->forceFlush(remainingTimeout(request->deadline)) && ok;
        }
        request->ok.store(ok, std::memory_order_release);
        request->done.release();
    };

    auto completeFlushRequests = [this, &completeFlushRequest] {
        bool completedAny = false;
        std::shared_ptr<WorkerControl::FlushRequest> request;
        while (m_control->flushRequests.try_dequeue(request)) {
            completeFlushRequest(request);
            completedAny = true;
        }
        return completedAny;
    };

    while (true) {
        const bool intervalElapsed = !m_control->waitFor(m_config.flush_interval);
        const bool shutdownRequested = m_control->shutdownRequested.load(std::memory_order_acquire);

        if (completeFlushRequests()) {
            continue;
        }

        if (shutdownRequested) {
            const auto shutdownDeadline = m_control->shutdownDeadline();
            bool shutdownOk = drainUntilIdle(shutdownDeadline);
            completeFlushRequests();
            const bool deadlineExpired = std::chrono::steady_clock::now() >= shutdownDeadline;
            if (shutdownRequested &&
                m_control->activeFlushCallers.load(std::memory_order_acquire) == 0 &&
                (m_queue->empty() || deadlineExpired) &&
                !completeFlushRequests()) {
                if (m_exporter && !deadlineExpired) {
                    shutdownOk = m_exporter->forceFlush(remainingTimeout(shutdownDeadline)) && shutdownOk;
                }
                m_control->shutdownDrainOk.store(shutdownOk && m_queue->empty(), std::memory_order_release);
                break;
            }
            continue;
        }

        const bool drainPartial = intervalElapsed || m_config.schedule_mode == BatchSpanScheduleMode::kOnEnd;
        if (!drainPartial && m_queue->queuedCount() < m_config.max_batch_size) {
            continue;
        }

        static_cast<void>(drainReadyBatches(drainPartial));
    }

    m_control->workerStopped.store(true, std::memory_order_release);
}

std::size_t BatchSpanProcessor::drainQueue(std::vector<Span>& batch, std::size_t maxCount) {
    return m_queue->tryPopBulk(batch, maxCount);
}

bool BatchSpanProcessor::exportBatch(std::span<const Span> spans) {
    if (spans.empty() || !m_exporter) {
        return true;
    }

    const bool success = m_exporter->exportSpans(spans) == ExportResult::kSuccess;
    if (!success) {
        TRACING_LOG_WARN("[batch_processor]", "export batch failed span_count={}", spans.size());
    } else if (TRACING_LOG_ENABLED(::galay::kernel::LogLevel::kDebug)) {
        TRACING_LOG_DEBUG("[batch_processor]", "export batch succeeded span_count={}", spans.size());
    }
    return success;
}

} // namespace galay::tracing
