#include <concurrentqueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kIterations = 100000;
constexpr std::size_t kBatchSize = 512;
constexpr std::size_t kRounds = 7;
constexpr std::string_view kSpanName = "batch-processor-benchmark-span";

struct QueueTraits : moodycamel::ConcurrentQueueDefaultTraits {
    static constexpr size_t BLOCK_SIZE = 512;
    static constexpr size_t IMPLICIT_INITIAL_INDEX_SIZE = 256;
    static constexpr size_t EXPLICIT_INITIAL_INDEX_SIZE = 256;
};

struct SpanPayload {
    std::array<std::uint8_t, 16> trace_id{};
    std::array<std::uint8_t, 8> span_id{};
    std::string name;
    std::string tracestate;
    std::vector<std::pair<std::string, std::int64_t>> attributes;
    std::string status_message;
    bool sampled{true};
};

struct QueueResult {
    double ns_per_send;
    double ns_per_flush;
    double ns_per_e2e;
};

[[nodiscard]] const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

[[nodiscard]] std::vector<SpanPayload> makePayloads() {
    std::vector<SpanPayload> payloads;
    payloads.reserve(kIterations);
    for (std::size_t i = 0; i < kIterations; ++i) {
        payloads.push_back({
            .trace_id = {0x4b},
            .span_id = {0x0f},
            .name = std::string(kSpanName),
            .tracestate = {},
            .attributes = {},
            .status_message = {},
            .sampled = true,
        });
    }
    return payloads;
}

[[nodiscard]] std::size_t spanWeight(const SpanPayload& span) noexcept {
    return span.trace_id.size() +
           span.span_id.size() +
           span.name.size() +
           span.tracestate.size() +
           span.attributes.size() +
           span.status_message.size();
}

class MoodycamelBatchQueue {
public:
    MoodycamelBatchQueue()
        : m_queue(kIterations),
          m_consumer(m_queue),
          m_worker(&MoodycamelBatchQueue::workerLoop, this) {
    }

    ~MoodycamelBatchQueue() {
        shutdown();
    }

    MoodycamelBatchQueue(const MoodycamelBatchQueue&) = delete;
    MoodycamelBatchQueue& operator=(const MoodycamelBatchQueue&) = delete;

    bool onEnd(SpanPayload span) {
        if (!span.sampled) {
            return true;
        }

        const auto previous = m_queued.fetch_add(1, std::memory_order_relaxed);
        if (previous >= kIterations) {
            m_queued.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        if (!m_queue.enqueue(std::move(span))) {
            m_queued.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        if (previous + 1 >= kBatchSize && previous < kBatchSize) {
            notify();
        }
        return true;
    }

    void forceFlush() {
        m_flushRequested.store(true, std::memory_order_release);
        forceNotify();
        m_flushDone.acquire();
    }

    void shutdown() {
        if (m_shutdown.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        forceNotify();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    [[nodiscard]] std::size_t exported() const noexcept {
        return m_exported.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t observedWeight() const noexcept {
        return m_observedWeight.load(std::memory_order_relaxed);
    }

private:
    void notify() noexcept {
        if (!m_signalPending.exchange(true, std::memory_order_acq_rel)) {
            m_signal.release();
        }
    }

    void forceNotify() noexcept {
        m_signalPending.store(true, std::memory_order_release);
        m_signal.release();
    }

    void workerLoop() {
        std::vector<SpanPayload> batch(kBatchSize);
        while (true) {
            m_signal.acquire();
            m_signalPending.store(false, std::memory_order_release);

            const bool flushRequested = m_flushRequested.exchange(false, std::memory_order_acq_rel);
            const bool shutdownRequested = m_shutdown.load(std::memory_order_acquire);
            drain(flushRequested || shutdownRequested, batch);
            if (flushRequested) {
                m_flushDone.release();
            }
            if (shutdownRequested) {
                drain(true, batch);
                break;
            }
        }
    }

    void drain(bool drainPartial, std::vector<SpanPayload>& batch) {
        while (m_queued.load(std::memory_order_relaxed) >= kBatchSize ||
               (drainPartial && m_queued.load(std::memory_order_relaxed) != 0)) {
            const auto count = m_queue.try_dequeue_bulk(m_consumer, batch.begin(), kBatchSize);
            if (count == 0) {
                break;
            }
            m_queued.fetch_sub(count, std::memory_order_relaxed);
            exportBatch(std::span<const SpanPayload>(batch.data(), count));
        }
    }

    void exportBatch(std::span<const SpanPayload> batch) {
        std::size_t observed = 0;
        for (const auto& span : batch) {
            observed += spanWeight(span);
        }
        m_observedWeight.fetch_add(observed, std::memory_order_relaxed);
        m_exported.fetch_add(batch.size(), std::memory_order_relaxed);
    }

    moodycamel::ConcurrentQueue<SpanPayload, QueueTraits> m_queue;
    moodycamel::ConsumerToken m_consumer;
    std::atomic<std::size_t> m_queued{0};
    std::atomic<std::size_t> m_exported{0};
    std::atomic<std::size_t> m_observedWeight{0};
    std::atomic<bool> m_signalPending{false};
    std::atomic<bool> m_flushRequested{false};
    std::atomic<bool> m_shutdown{false};
    std::counting_semaphore<> m_signal{0};
    std::binary_semaphore m_flushDone{0};
    std::thread m_worker;
};

QueueResult runOnce() {
    auto payloads = makePayloads();
    MoodycamelBatchQueue queue;

    const auto start = std::chrono::steady_clock::now();
    bool accepted = true;
    for (auto& payload : payloads) {
        accepted = queue.onEnd(std::move(payload)) && accepted;
    }
    const auto afterSend = std::chrono::steady_clock::now();
    queue.forceFlush();
    const auto afterFlush = std::chrono::steady_clock::now();
    queue.shutdown();

    const auto sendNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterSend - start).count();
    const auto flushNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterFlush - afterSend).count();
    const auto totalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(afterFlush - start).count();
    const bool ok = accepted && queue.exported() == kIterations && queue.observedWeight() != 0;
    if (!ok) {
        std::cout << "B8-MoodycamelBatchQueue workload=" << kIterations
                  << " build=" << buildType()
                  << " ok=0 exported=" << queue.exported()
                  << " observed_weight=" << queue.observedWeight() << '\n';
    }

    return {
        .ns_per_send = static_cast<double>(sendNs) / kIterations,
        .ns_per_flush = static_cast<double>(flushNs) / kIterations,
        .ns_per_e2e = static_cast<double>(totalNs) / kIterations,
    };
}

void runBenchmark() {
    std::array<QueueResult, kRounds> results{};
    for (auto& result : results) {
        result = runOnce();
    }

    auto best = [](std::array<QueueResult, kRounds> values, auto member) {
        return std::ranges::min(values, {}, member).*member;
    };
    auto median = [](std::array<QueueResult, kRounds> values, auto member) {
        std::ranges::sort(values, {}, member);
        return values[kRounds / 2].*member;
    };

    std::cout << "B8-MoodycamelBatchQueue workload=" << kIterations
              << " build=" << buildType()
              << " library=moodycamel"
              << " payload=span_like_owned"
              << " rounds=" << kRounds
              << " batch_size=" << kBatchSize
              << " queue_capacity=" << kIterations
              << " payload_name_len=" << kSpanName.size()
              << " best_send_ns=" << best(results, &QueueResult::ns_per_send)
              << " median_send_ns=" << median(results, &QueueResult::ns_per_send)
              << " best_flush_ns=" << best(results, &QueueResult::ns_per_flush)
              << " median_flush_ns=" << median(results, &QueueResult::ns_per_flush)
              << " best_e2e_ns=" << best(results, &QueueResult::ns_per_e2e)
              << " median_e2e_ns=" << median(results, &QueueResult::ns_per_e2e)
              << '\n';
}

} // namespace

int main() {
    runBenchmark();
}
