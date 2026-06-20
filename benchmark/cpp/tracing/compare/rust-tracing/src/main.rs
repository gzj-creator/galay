use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::thread;
use std::thread::Thread;
use std::time::Instant;

use crossbeam_channel::{bounded, Receiver, Sender};
use crossbeam_queue::ArrayQueue;
use serde_json::json;
use tracing::span::{Attributes, Record};
use tracing::subscriber::with_default;
use tracing::{debug, info, span, Event, Id, Level, Metadata, Subscriber};

const LOG_ITERATIONS: u64 = 200_000;
const ENABLED_ITERATIONS: u64 = 100_000;
const SPAN_ITERATIONS: u64 = 20_000;
const OTLP_ITERATIONS: u64 = 10_000;
const OTLP_BATCH_SIZE: u64 = 8;
const BATCH_PROCESSOR_ITERATIONS: usize = 100_000;
const BATCH_PROCESSOR_BATCH_SIZE: usize = 512;
const BATCH_PROCESSOR_ROUNDS: usize = 7;
const BATCH_PROCESSOR_SPAN_NAME: &str = "batch-processor-benchmark-span";

struct NoopSubscriber {
    enabled: bool,
    next_id: AtomicU64,
}

impl NoopSubscriber {
    fn enabled() -> Self {
        Self {
            enabled: true,
            next_id: AtomicU64::new(1),
        }
    }

    fn disabled() -> Self {
        Self {
            enabled: false,
            next_id: AtomicU64::new(1),
        }
    }
}

impl Subscriber for NoopSubscriber {
    fn enabled(&self, _metadata: &Metadata<'_>) -> bool {
        self.enabled
    }

    fn new_span(&self, _span: &Attributes<'_>) -> Id {
        Id::from_u64(self.next_id.fetch_add(1, Ordering::Relaxed))
    }

    fn record(&self, _span: &Id, _values: &Record<'_>) {}

    fn record_follows_from(&self, _span: &Id, _follows: &Id) {}

    fn event(&self, _event: &Event<'_>) {}

    fn enter(&self, _span: &Id) {}

    fn exit(&self, _span: &Id) {}
}

fn measure_disabled_event() {
    with_default(NoopSubscriber::disabled(), || {
        let start = Instant::now();
        for i in 0..LOG_ITERATIONS {
            debug!(value = black_box(i), "value");
        }
        let ns = start.elapsed().as_nanos() as f64;
        println!(
            "R1-RustTracingDisabled workload={} build=Release framework=tracing ns_per_event={}",
            LOG_ITERATIONS,
            ns / LOG_ITERATIONS as f64
        );
    });
}

fn measure_enabled_noop_event() {
    with_default(NoopSubscriber::enabled(), || {
        let start = Instant::now();
        for i in 0..ENABLED_ITERATIONS {
            info!(value = black_box(i), "value");
        }
        let ns = start.elapsed().as_nanos() as f64;
        println!(
            "R2-RustTracingEnabledNoop workload={} build=Release framework=tracing ns_per_event={}",
            ENABLED_ITERATIONS,
            ns / ENABLED_ITERATIONS as f64
        );
    });
}

fn measure_span_scope() {
    with_default(NoopSubscriber::enabled(), || {
        let start = Instant::now();
        for _ in 0..SPAN_ITERATIONS {
            let current = span!(Level::INFO, "bench");
            let guard = current.enter();
            black_box(&guard);
        }
        let ns = start.elapsed().as_nanos() as f64;
        println!(
            "R3-RustTracingSpanScopeNoop workload={} build=Release framework=tracing ns_per_scope={}",
            SPAN_ITERATIONS,
            ns / SPAN_ITERATIONS as f64
        );
    });
}

fn measure_serde_json_otlp() {
    let span_ids = [
        "00f067aa0ba902b7",
        "00f067aa0ba902b8",
        "00f067aa0ba902b9",
        "00f067aa0ba902ba",
        "00f067aa0ba902bb",
        "00f067aa0ba902bc",
        "00f067aa0ba902bd",
        "00f067aa0ba902be",
    ];
    let start = Instant::now();
    let mut bytes = 0usize;
    for _ in 0..OTLP_ITERATIONS {
        let spans = span_ids
            .iter()
            .map(|span_id| {
                json!({
                    "traceId": "4bf92f3577b34da6a3ce929d0e0e4736",
                    "spanId": span_id,
                    "name": "otlp-bench-span",
                    "kind": "SPAN_KIND_INTERNAL",
                    "traceState": "vendor=value"
                })
            })
            .collect::<Vec<_>>();
        let body = json!({
            "resourceSpans": [{
                "scopeSpans": [{
                    "scope": {"name": "galay-tracing"},
                    "spans": spans
                }]
            }]
        })
        .to_string();
        bytes += black_box(body.len());
    }
    let ns = start.elapsed().as_nanos() as f64;
    println!(
        "R4-RustSerdeJsonOtlp workload={} batch_size={} build=Release framework=serde_json ns_per_span={} avg_body_bytes={}",
        OTLP_ITERATIONS,
        OTLP_BATCH_SIZE,
        ns / (OTLP_ITERATIONS * OTLP_BATCH_SIZE) as f64,
        bytes / OTLP_ITERATIONS as usize
    );
}

#[derive(Clone)]
struct SpanPayload {
    trace_id: [u8; 16],
    span_id: [u8; 8],
    name: String,
    tracestate: String,
    attributes: Vec<(String, i64)>,
    status_message: String,
    sampled: bool,
}

impl SpanPayload {
    fn sampled(&self) -> bool {
        self.sampled
    }
}

enum BatchMessage {
    Span(SpanPayload),
    Flush(crossbeam_channel::Sender<()>),
    Shutdown,
}

#[derive(Clone, Copy)]
struct BatchProcessorResult {
    ns_per_send: f64,
    ns_per_flush: f64,
    ns_per_e2e: f64,
    exported: u64,
    ok: bool,
}

fn best_metric(
    results: &[BatchProcessorResult],
    metric: impl Fn(&BatchProcessorResult) -> f64,
) -> f64 {
    results
        .iter()
        .map(metric)
        .fold(f64::INFINITY, |best, value| best.min(value))
}

fn median_metric(
    results: &[BatchProcessorResult],
    metric: impl Fn(&BatchProcessorResult) -> f64,
) -> f64 {
    let mut values = results.iter().map(metric).collect::<Vec<_>>();
    values.sort_by(f64::total_cmp);
    values[values.len() / 2]
}

fn drain_available(
    receiver: &Receiver<BatchMessage>,
    batch: &mut Vec<SpanPayload>,
    exported: &AtomicU64,
) {
    while batch.len() < BATCH_PROCESSOR_BATCH_SIZE {
        match receiver.try_recv() {
            Ok(BatchMessage::Span(span)) => batch.push(span),
            Ok(BatchMessage::Flush(done)) => {
                black_box(batch.iter().map(span_weight).sum::<usize>());
                exported.fetch_add(batch.len() as u64, Ordering::Relaxed);
                batch.clear();
                let _ = done.send(());
            }
            Ok(BatchMessage::Shutdown) => break,
            Err(_) => break,
        }
    }
    if batch.len() >= BATCH_PROCESSOR_BATCH_SIZE {
        black_box(batch.iter().map(span_weight).sum::<usize>());
        exported.fetch_add(batch.len() as u64, Ordering::Relaxed);
        batch.clear();
    }
}

fn spawn_batch_worker(
    receiver: Receiver<BatchMessage>,
    exported: Arc<AtomicU64>,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let mut batch = Vec::with_capacity(BATCH_PROCESSOR_BATCH_SIZE);
        while let Ok(message) = receiver.recv() {
            match message {
                BatchMessage::Span(span) => {
                    batch.push(span);
                    drain_available(&receiver, &mut batch, &exported);
                }
                BatchMessage::Flush(done) => {
                    loop {
                        match receiver.try_recv() {
                            Ok(BatchMessage::Span(span)) => batch.push(span),
                            Ok(BatchMessage::Flush(other_done)) => {
                                black_box(batch.iter().map(span_weight).sum::<usize>());
                                exported.fetch_add(batch.len() as u64, Ordering::Relaxed);
                                batch.clear();
                                let _ = other_done.send(());
                            }
                            Ok(BatchMessage::Shutdown) | Err(_) => break,
                        }
                    }
                    black_box(batch.iter().map(span_weight).sum::<usize>());
                    exported.fetch_add(batch.len() as u64, Ordering::Relaxed);
                    batch.clear();
                    let _ = done.send(());
                }
                BatchMessage::Shutdown => break,
            }
        }
        black_box(batch.iter().map(span_weight).sum::<usize>());
        exported.fetch_add(batch.len() as u64, Ordering::Relaxed);
    })
}

fn span_weight(span: &SpanPayload) -> usize {
    span.trace_id.len()
        + span.span_id.len()
        + span.name.len()
        + span.tracestate.len()
        + span.status_message.len()
        + span.attributes.len()
}

fn make_payloads() -> Vec<SpanPayload> {
    (0..BATCH_PROCESSOR_ITERATIONS)
        .map(|_| SpanPayload {
            trace_id: [0x4b; 16],
            span_id: [0x0f; 8],
            name: BATCH_PROCESSOR_SPAN_NAME.to_string(),
            tracestate: String::new(),
            attributes: Vec::new(),
            status_message: String::new(),
            sampled: true,
        })
        .collect()
}

fn send_span(sender: &Sender<BatchMessage>, span: SpanPayload) {
    if span.sampled {
        sender.send(BatchMessage::Span(span)).unwrap();
    }
}

fn run_crossbeam_batch_processor_once() -> BatchProcessorResult {
    let payloads = make_payloads();
    let (sender, receiver) = bounded::<BatchMessage>(BATCH_PROCESSOR_ITERATIONS + 2);
    let exported = Arc::new(AtomicU64::new(0));
    let worker = spawn_batch_worker(receiver, Arc::clone(&exported));

    let start = Instant::now();
    for span in payloads {
        black_box(send_span(&sender, span));
    }
    let after_send = Instant::now();
    let (flush_sender, flush_receiver) = bounded(1);
    sender.send(BatchMessage::Flush(flush_sender)).unwrap();
    flush_receiver.recv().unwrap();
    let after_flush = Instant::now();
    sender.send(BatchMessage::Shutdown).unwrap();
    worker.join().unwrap();

    let send_ns = (after_send - start).as_nanos() as f64;
    let flush_ns = (after_flush - after_send).as_nanos() as f64;
    let total_ns = (after_flush - start).as_nanos() as f64;
    let exported_count = exported.load(Ordering::Relaxed);
    BatchProcessorResult {
        ns_per_send: send_ns / BATCH_PROCESSOR_ITERATIONS as f64,
        ns_per_flush: flush_ns / BATCH_PROCESSOR_ITERATIONS as f64,
        ns_per_e2e: total_ns / BATCH_PROCESSOR_ITERATIONS as f64,
        exported: exported_count,
        ok: exported_count == BATCH_PROCESSOR_ITERATIONS as u64,
    }
}

fn measure_crossbeam_batch_processor() {
    let results = (0..BATCH_PROCESSOR_ROUNDS)
        .map(|_| run_crossbeam_batch_processor_once())
        .collect::<Vec<_>>();
    let ok = results.iter().all(|result| result.ok);
    println!(
        "R5-RustCrossbeamBatchProcessor workload={} build=Release framework=crossbeam-channel payload=span_like_owned rounds={} batch_size={} queue_capacity={} payload_name_len={} best_send_ns={} median_send_ns={} best_flush_ns={} median_flush_ns={} best_e2e_ns={} median_e2e_ns={} exported_each={} ok={}",
        BATCH_PROCESSOR_ITERATIONS,
        BATCH_PROCESSOR_ROUNDS,
        BATCH_PROCESSOR_BATCH_SIZE,
        BATCH_PROCESSOR_ITERATIONS,
        BATCH_PROCESSOR_SPAN_NAME.len(),
        best_metric(&results, |result| result.ns_per_send),
        median_metric(&results, |result| result.ns_per_send),
        best_metric(&results, |result| result.ns_per_flush),
        median_metric(&results, |result| result.ns_per_flush),
        best_metric(&results, |result| result.ns_per_e2e),
        median_metric(&results, |result| result.ns_per_e2e),
        results.last().map_or(0, |result| result.exported),
        ok
    );
}

enum QueueMessage {
    Span(SpanPayload),
    Flush(crossbeam_channel::Sender<()>),
    Shutdown,
}

struct ArrayQueueBatchProcessor {
    queue: Arc<ArrayQueue<QueueMessage>>,
    queued_spans: Arc<AtomicU64>,
    exported: Arc<AtomicU64>,
    worker_thread: Thread,
    worker: Option<thread::JoinHandle<()>>,
}

impl ArrayQueueBatchProcessor {
    fn new() -> Self {
        let queue = Arc::new(ArrayQueue::new(BATCH_PROCESSOR_ITERATIONS + 2));
        let queued_spans = Arc::new(AtomicU64::new(0));
        let exported = Arc::new(AtomicU64::new(0));
        let (ready_sender, ready_receiver) = bounded(1);

        let worker_queue = Arc::clone(&queue);
        let worker_queued_spans = Arc::clone(&queued_spans);
        let worker_exported = Arc::clone(&exported);
        let worker = thread::spawn(move || {
            let current = thread::current();
            ready_sender.send(current).unwrap();
            let mut batch = Vec::with_capacity(BATCH_PROCESSOR_BATCH_SIZE);

            loop {
                thread::park();
                let mut should_stop = false;
                drain_array_queue(
                    &worker_queue,
                    &worker_queued_spans,
                    &worker_exported,
                    &mut batch,
                    &mut should_stop,
                    false,
                );
                if should_stop {
                    drain_array_queue(
                        &worker_queue,
                        &worker_queued_spans,
                        &worker_exported,
                        &mut batch,
                        &mut should_stop,
                        true,
                    );
                    export_payload_batch(&mut batch, &worker_exported);
                    break;
                }
            }
        });

        Self {
            queue,
            queued_spans,
            exported,
            worker_thread: ready_receiver.recv().unwrap(),
            worker: Some(worker),
        }
    }

    fn on_end(&self, span: SpanPayload) {
        if !span.sampled() {
            return;
        }

        let previous = self.queued_spans.fetch_add(1, Ordering::Relaxed);
        if previous as usize >= BATCH_PROCESSOR_ITERATIONS {
            self.queued_spans.fetch_sub(1, Ordering::Relaxed);
            return;
        }

        if self.queue.push(QueueMessage::Span(span)).is_err() {
            self.queued_spans.fetch_sub(1, Ordering::Relaxed);
            return;
        }
        if previous as usize + 1 >= BATCH_PROCESSOR_BATCH_SIZE
            && (previous as usize) < BATCH_PROCESSOR_BATCH_SIZE
        {
            self.worker_thread.unpark();
        }
    }

    fn flush(&self) {
        let (flush_sender, flush_receiver) = bounded(1);
        self.push_control(QueueMessage::Flush(flush_sender));
        self.worker_thread.unpark();
        flush_receiver.recv().unwrap();
    }

    fn shutdown(&mut self) {
        self.push_control(QueueMessage::Shutdown);
        self.worker_thread.unpark();
        if let Some(worker) = self.worker.take() {
            worker.join().unwrap();
        }
    }

    fn exported(&self) -> u64 {
        self.exported.load(Ordering::Relaxed)
    }

    fn push_control(&self, mut message: QueueMessage) {
        loop {
            match self.queue.push(message) {
                Ok(()) => return,
                Err(returned) => {
                    message = returned;
                    self.worker_thread.unpark();
                    thread::yield_now();
                }
            }
        }
    }
}

fn drain_array_queue(
    queue: &ArrayQueue<QueueMessage>,
    queued_spans: &AtomicU64,
    exported: &AtomicU64,
    batch: &mut Vec<SpanPayload>,
    should_stop: &mut bool,
    drain_partial: bool,
) {
    loop {
        while batch.len() < BATCH_PROCESSOR_BATCH_SIZE {
            match queue.pop() {
                Some(QueueMessage::Span(span)) => {
                    queued_spans.fetch_sub(1, Ordering::Relaxed);
                    batch.push(span);
                }
                Some(QueueMessage::Flush(done)) => {
                    drain_array_queue(queue, queued_spans, exported, batch, should_stop, true);
                    export_payload_batch(batch, exported);
                    let _ = done.send(());
                }
                Some(QueueMessage::Shutdown) => {
                    *should_stop = true;
                    return;
                }
                None => break,
            }
        }

        if batch.len() >= BATCH_PROCESSOR_BATCH_SIZE {
            export_payload_batch(batch, exported);
            continue;
        }

        if !drain_partial || queue.is_empty() {
            break;
        }
    }
}

fn export_payload_batch(batch: &mut Vec<SpanPayload>, exported: &AtomicU64) {
    if batch.is_empty() {
        return;
    }
    black_box(batch.iter().map(span_weight).sum::<usize>());
    exported.fetch_add(batch.len() as u64, Ordering::Relaxed);
    batch.clear();
}

fn run_array_queue_batch_size_processor_once() -> BatchProcessorResult {
    let payloads = make_payloads();
    let mut processor = ArrayQueueBatchProcessor::new();

    let start = Instant::now();
    for span in payloads {
        black_box(processor.on_end(span));
    }
    let after_send = Instant::now();
    processor.flush();
    let after_flush = Instant::now();
    processor.shutdown();

    let send_ns = (after_send - start).as_nanos() as f64;
    let flush_ns = (after_flush - after_send).as_nanos() as f64;
    let total_ns = (after_flush - start).as_nanos() as f64;
    let exported_count = processor.exported();
    BatchProcessorResult {
        ns_per_send: send_ns / BATCH_PROCESSOR_ITERATIONS as f64,
        ns_per_flush: flush_ns / BATCH_PROCESSOR_ITERATIONS as f64,
        ns_per_e2e: total_ns / BATCH_PROCESSOR_ITERATIONS as f64,
        exported: exported_count,
        ok: exported_count == BATCH_PROCESSOR_ITERATIONS as u64,
    }
}

fn measure_array_queue_batch_size_processor() {
    let results = (0..BATCH_PROCESSOR_ROUNDS)
        .map(|_| run_array_queue_batch_size_processor_once())
        .collect::<Vec<_>>();
    let ok = results.iter().all(|result| result.ok);
    println!(
        "R6-RustCrossbeamArrayQueueBatchSize workload={} build=Release framework=crossbeam-queue payload=span_like_owned rounds={} batch_size={} queue_capacity={} payload_name_len={} best_send_ns={} median_send_ns={} best_flush_ns={} median_flush_ns={} best_e2e_ns={} median_e2e_ns={} exported_each={} ok={}",
        BATCH_PROCESSOR_ITERATIONS,
        BATCH_PROCESSOR_ROUNDS,
        BATCH_PROCESSOR_BATCH_SIZE,
        BATCH_PROCESSOR_ITERATIONS,
        BATCH_PROCESSOR_SPAN_NAME.len(),
        best_metric(&results, |result| result.ns_per_send),
        median_metric(&results, |result| result.ns_per_send),
        best_metric(&results, |result| result.ns_per_flush),
        median_metric(&results, |result| result.ns_per_flush),
        best_metric(&results, |result| result.ns_per_e2e),
        median_metric(&results, |result| result.ns_per_e2e),
        results.last().map_or(0, |result| result.exported),
        ok
    );
}

fn main() {
    measure_disabled_event();
    measure_enabled_noop_event();
    measure_span_scope();
    measure_serde_json_otlp();
    measure_crossbeam_batch_processor();
    measure_array_queue_batch_size_processor();
}
