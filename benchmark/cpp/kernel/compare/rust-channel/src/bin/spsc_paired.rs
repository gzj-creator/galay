use rtrb::{PopError, PushError, RingBuffer};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

#[derive(Clone, Copy, PartialEq, Eq)]
enum CaseKind {
    RawBounded,
    BatchBounded,
    Unbounded,
    BatchUnbounded,
}

impl CaseKind {
    fn name(self) -> &'static str {
        match self {
            Self::RawBounded => "raw_bounded",
            Self::BatchBounded => "batch_bounded",
            Self::Unbounded => "unbounded",
            Self::BatchUnbounded => "batch_unbounded",
        }
    }

    fn implementation(self) -> &'static str {
        match self {
            Self::RawBounded => "rtrb::RingBuffer@0.3.4",
            Self::BatchBounded => "rtrb::RingBuffer@0.3.4",
            Self::Unbounded => "unbounded_spsc::channel@0.3.0",
            Self::BatchUnbounded => "unbounded_spsc::channel@0.3.0 scalar batch emulation",
        }
    }

    fn api_profile(self) -> &'static str {
        match self {
            Self::RawBounded => "bounded_spsc_polling_split",
            Self::BatchBounded => "bounded_spsc_batch_polling_split",
            Self::Unbounded => "unbounded_spsc_wait_capable_polling_path",
            Self::BatchUnbounded => "unbounded_spsc_batch_polling",
        }
    }

    fn comparison_scope(self) -> &'static str {
        match self {
            Self::RawBounded => "equivalent_measured_api",
            Self::BatchBounded => "equivalent_measured_api",
            Self::Unbounded => "nearest_available_measured_path",
            Self::BatchUnbounded => "reference_only_no_equivalent_rust_batch_api",
        }
    }

    fn is_bounded(self) -> bool {
        matches!(self, Self::RawBounded | Self::BatchBounded)
    }

    fn is_batch(self) -> bool {
        matches!(self, Self::BatchBounded | Self::BatchUnbounded)
    }
}

#[derive(Clone, Copy)]
enum BackoffKind {
    Yield,
    Spin,
    Hybrid,
}

impl BackoffKind {
    fn name(self) -> &'static str {
        match self {
            Self::Yield => "yield",
            Self::Spin => "spin",
            Self::Hybrid => "hybrid",
        }
    }
}

struct Backoff {
    failures: usize,
    kind: BackoffKind,
}

impl Backoff {
    fn new(kind: BackoffKind) -> Self {
        Self { failures: 0, kind }
    }

    fn reset(&mut self) {
        self.failures = 0;
    }

    fn wait(&mut self) {
        const RETRY_LIMIT: usize = 64;
        const MAXIMUM_SPIN_EXPONENT: usize = 6;
        match self.kind {
            BackoffKind::Yield => thread::yield_now(),
            BackoffKind::Spin => {
                if self.failures < RETRY_LIMIT {
                    let pauses = 1_usize << self.failures.min(MAXIMUM_SPIN_EXPONENT);
                    for _ in 0..pauses {
                        std::hint::spin_loop();
                    }
                } else {
                    thread::yield_now();
                }
            }
            BackoffKind::Hybrid => {
                if self.failures < RETRY_LIMIT {
                    std::hint::spin_loop();
                } else {
                    thread::yield_now();
                }
            }
        }
        self.failures = self.failures.saturating_add(1);
    }
}

struct Config {
    messages: u64,
    capacity: usize,
    batch_size: usize,
    producer_core: usize,
    consumer_core: usize,
    kind: CaseKind,
    backoff: BackoffKind,
}

struct ProducerResult {
    full_retries: u64,
    send_ok: bool,
}

struct ConsumerResult {
    received: u64,
    checksum: u64,
    empty_retries: u64,
    fifo_ok: bool,
}

struct Measurement {
    messages_per_second: f64,
    elapsed_ns: u64,
    received: u64,
    checksum: u64,
    expected_checksum: u64,
    full_retries: u64,
    empty_retries: u64,
    producer_placement: ThreadPlacement,
    consumer_placement: ThreadPlacement,
    fifo_ok: bool,
    send_ok: bool,
}

struct StartState {
    ready: AtomicUsize,
    start: AtomicBool,
    producer_finished: AtomicBool,
    consumer_done: AtomicBool,
}

impl StartState {
    fn new() -> Self {
        Self {
            ready: AtomicUsize::new(0),
            start: AtomicBool::new(false),
            producer_finished: AtomicBool::new(false),
            consumer_done: AtomicBool::new(false),
        }
    }
}

#[derive(Clone, Copy)]
#[allow(dead_code)]
enum ThreadPlacement {
    PinnedToCore,
    PerformanceClassOnly,
    AffinityHintOnly,
    Unsupported,
}

impl ThreadPlacement {
    fn name(self) -> &'static str {
        match self {
            Self::PinnedToCore => "pinned",
            Self::PerformanceClassOnly => "perf-class-only",
            Self::AffinityHintOnly => "affinity-hint-only",
            Self::Unsupported => "unsupported",
        }
    }
}

#[cfg(target_os = "macos")]
fn pin_current_thread(core_index: usize) -> ThreadPlacement {
    use std::ffi::c_void;

    type Pthread = *mut c_void;

    extern "C" {
        fn pthread_self() -> Pthread;
        fn pthread_mach_thread_np(thread: Pthread) -> u32;
        fn thread_policy_set(thread: u32, flavor: i32, policy_info: *mut i32, count: u32) -> i32;
        fn pthread_set_qos_class_self_np(qos_class: u32, relative_priority: i32) -> i32;
    }

    const THREAD_AFFINITY_POLICY: i32 = 4;
    const QOS_CLASS_USER_INTERACTIVE: u32 = 0x21;
    let online = thread::available_parallelism()
        .map(|count| count.get())
        .unwrap_or(1);
    let target = core_index % online;
    let mut policy = [(target + 1) as i32];

    // SAFETY: declarations match the Darwin pthread/Mach ABIs, and policy
    // contains one integer for THREAD_AFFINITY_POLICY_COUNT.
    unsafe {
        let mach_thread = pthread_mach_thread_np(pthread_self());
        let affinity_hint = thread_policy_set(
            mach_thread,
            THREAD_AFFINITY_POLICY,
            policy.as_mut_ptr(),
            policy.len() as u32,
        ) == 0;
        if pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0 {
            return ThreadPlacement::PerformanceClassOnly;
        }
        if affinity_hint {
            return ThreadPlacement::AffinityHintOnly;
        }
    }
    ThreadPlacement::Unsupported
}

#[cfg(target_os = "linux")]
fn pin_current_thread(core_index: usize) -> ThreadPlacement {
    extern "C" {
        fn pthread_self() -> usize;
        fn pthread_setaffinity_np(thread: usize, cpuset_size: usize, cpuset: *const u8) -> i32;
    }

    let online = thread::available_parallelism()
        .map(|count| count.get())
        .unwrap_or(1);
    let target = core_index % online;
    let mut cpuset = [0_u8; 128];
    if target >= cpuset.len() * 8 {
        return ThreadPlacement::Unsupported;
    }
    cpuset[target / 8] |= 1_u8 << (target % 8);

    // SAFETY: cpuset is a Linux cpu_set_t-sized initialized bitmap and remains
    // alive for the complete pthread_setaffinity_np call.
    let result = unsafe { pthread_setaffinity_np(pthread_self(), cpuset.len(), cpuset.as_ptr()) };
    if result == 0 {
        ThreadPlacement::PinnedToCore
    } else {
        ThreadPlacement::Unsupported
    }
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn pin_current_thread(_core_index: usize) -> ThreadPlacement {
    ThreadPlacement::Unsupported
}

fn parse_arguments() -> Result<Config, String> {
    let arguments = std::env::args().skip(1).collect::<Vec<_>>();
    if arguments.len() % 2 != 0 {
        return Err("missing option value".to_owned());
    }

    let mut config = Config {
        messages: 1_000_000,
        capacity: 4096,
        batch_size: 64,
        producer_core: 0,
        consumer_core: 1,
        kind: CaseKind::RawBounded,
        backoff: BackoffKind::Yield,
    };
    for pair in arguments.chunks_exact(2) {
        match pair[0].as_str() {
            "--case" => {
                config.kind = match pair[1].as_str() {
                    "raw_bounded" => CaseKind::RawBounded,
                    "batch_bounded" => CaseKind::BatchBounded,
                    "unbounded" => CaseKind::Unbounded,
                    "batch_unbounded" => CaseKind::BatchUnbounded,
                    _ => return Err("invalid case".to_owned()),
                };
            }
            "--messages" => {
                config.messages = pair[1]
                    .parse::<u64>()
                    .map_err(|_| "invalid message count".to_owned())?;
            }
            "--capacity" => {
                config.capacity = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid capacity".to_owned())?;
            }
            "--batch-size" => {
                config.batch_size = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid batch size".to_owned())?;
            }
            "--backoff" => {
                config.backoff = match pair[1].as_str() {
                    "yield" => BackoffKind::Yield,
                    "spin" => BackoffKind::Spin,
                    "hybrid" => BackoffKind::Hybrid,
                    _ => return Err("backoff must be yield, spin, or hybrid".to_owned()),
                };
            }
            "--producer-core" => {
                config.producer_core = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid producer core".to_owned())?;
            }
            "--consumer-core" => {
                config.consumer_core = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid consumer core".to_owned())?;
            }
            _ => return Err(format!("unknown option: {}", pair[0])),
        }
    }

    if config.messages == 0 {
        return Err("message count must be positive".to_owned());
    }
    if config.kind.is_bounded() && (config.capacity < 2 || !config.capacity.is_power_of_two()) {
        return Err("bounded capacity must be a power of two and at least 2".to_owned());
    }
    if config.kind.is_batch() && config.batch_size == 0 {
        return Err("batch size must be positive".to_owned());
    }
    if config.producer_core == config.consumer_core {
        return Err("producer and consumer cores must differ".to_owned());
    }
    Ok(config)
}

fn expected_checksum(messages: u64) -> u64 {
    ((messages as u128 * messages.saturating_sub(1) as u128 / 2) & u64::MAX as u128) as u64
}

fn spawn_worker<T, F>(
    core_index: usize,
    state: Arc<StartState>,
    work: F,
) -> thread::JoinHandle<(ThreadPlacement, T)>
where
    T: Send + 'static,
    F: FnOnce() -> T + Send + 'static,
{
    thread::spawn(move || {
        let placement = pin_current_thread(core_index);
        state.ready.fetch_add(1, Ordering::Release);
        while !state.start.load(Ordering::Acquire) {
            thread::yield_now();
        }
        (placement, work())
    })
}

fn finish_measurement(
    config: &Config,
    begin: Instant,
    producer: thread::JoinHandle<(ThreadPlacement, ProducerResult)>,
    consumer: thread::JoinHandle<(ThreadPlacement, ConsumerResult)>,
) -> Result<Measurement, String> {
    let (producer_placement, producer_result) = producer
        .join()
        .map_err(|_| "producer thread panicked".to_owned())?;
    let (consumer_placement, consumer_result) = consumer
        .join()
        .map_err(|_| "consumer thread panicked".to_owned())?;
    let elapsed_ns = begin.elapsed().as_nanos() as u64;
    Ok(Measurement {
        messages_per_second: if elapsed_ns > 0 {
            config.messages as f64 * 1_000_000_000.0 / elapsed_ns as f64
        } else {
            0.0
        },
        elapsed_ns,
        received: consumer_result.received,
        checksum: consumer_result.checksum,
        expected_checksum: expected_checksum(config.messages),
        full_retries: producer_result.full_retries,
        empty_retries: consumer_result.empty_retries,
        producer_placement,
        consumer_placement,
        fifo_ok: consumer_result.fifo_ok,
        send_ok: producer_result.send_ok,
    })
}

fn run_bounded(config: &Config) -> Result<Measurement, String> {
    let (mut producer_queue, mut consumer_queue) = RingBuffer::<u64>::new(config.capacity);
    let state = Arc::new(StartState::new());

    let messages = config.messages;
    let backoff_kind = config.backoff;
    let producer = spawn_worker(config.producer_core, Arc::clone(&state), move || {
        let mut full_retries = 0_u64;
        let mut backoff = Backoff::new(backoff_kind);
        for sequence in 0..messages {
            let mut pending = sequence;
            loop {
                match producer_queue.push(pending) {
                    Ok(()) => break,
                    Err(PushError::Full(returned)) => {
                        pending = returned;
                        full_retries += 1;
                        backoff.wait();
                    }
                }
            }
            backoff.reset();
        }
        ProducerResult {
            full_retries,
            send_ok: true,
        }
    });

    let backoff_kind = config.backoff;
    let consumer = spawn_worker(config.consumer_core, Arc::clone(&state), move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        let mut backoff = Backoff::new(backoff_kind);
        while result.received < messages {
            match consumer_queue.pop() {
                Ok(value) => {
                    result.fifo_ok &= value == result.received;
                    result.checksum = result.checksum.wrapping_add(value);
                    result.received += 1;
                    backoff.reset();
                    continue;
                }
                Err(PopError::Empty) => {}
            }
            result.empty_retries += 1;
            // Drain to the expected count so a transient empty pop after
            // producer completion cannot be reported as data loss.
            backoff.wait();
        }
        result
    });

    while state.ready.load(Ordering::Acquire) != 2 {
        thread::yield_now();
    }
    let begin = Instant::now();
    state.start.store(true, Ordering::Release);
    finish_measurement(config, begin, producer, consumer)
}

fn run_unbounded(config: &Config) -> Result<Measurement, String> {
    let (producer_queue, consumer_queue) = unbounded_spsc::channel::<u64>();
    let state = Arc::new(StartState::new());

    let messages = config.messages;
    let producer_finish_state = Arc::clone(&state);
    let producer = spawn_worker(config.producer_core, Arc::clone(&state), move || {
        for sequence in 0..messages {
            if producer_queue.send(sequence).is_err() {
                producer_finish_state
                    .producer_finished
                    .store(true, Ordering::Release);
                return ProducerResult {
                    full_retries: 0,
                    send_ok: false,
                };
            }
        }
        producer_finish_state
            .producer_finished
            .store(true, Ordering::Release);
        while !producer_finish_state.consumer_done.load(Ordering::Acquire) {
            thread::yield_now();
        }
        ProducerResult {
            full_retries: 0,
            send_ok: true,
        }
    });

    let backoff_kind = config.backoff;
    let consumer_finish_state = Arc::clone(&state);
    let consumer = spawn_worker(config.consumer_core, Arc::clone(&state), move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        let mut backoff = Backoff::new(backoff_kind);
        let mut stalled_since: Option<Instant> = None;
        while result.received < messages {
            match consumer_queue.try_recv() {
                Ok(value) => {
                    stalled_since = None;
                    result.fifo_ok &= value == result.received;
                    result.checksum = result.checksum.wrapping_add(value);
                    result.received += 1;
                    backoff.reset();
                    continue;
                }
                Err(unbounded_spsc::TryRecvError::Empty)
                | Err(unbounded_spsc::TryRecvError::Disconnected) => {
                    if consumer_finish_state
                        .producer_finished
                        .load(Ordering::Acquire)
                    {
                        let since = *stalled_since.get_or_insert_with(Instant::now);
                        if since.elapsed() >= std::time::Duration::from_secs(1) {
                            result.fifo_ok = false;
                            break;
                        }
                    }
                }
            }
            result.empty_retries += 1;
            // Drain to the expected count so a transient empty pop after
            // producer completion cannot be reported as data loss.
            backoff.wait();
        }
        consumer_finish_state
            .consumer_done
            .store(true, Ordering::Release);
        result
    });

    while state.ready.load(Ordering::Acquire) != 2 {
        thread::yield_now();
    }
    let begin = Instant::now();
    state.start.store(true, Ordering::Release);
    finish_measurement(config, begin, producer, consumer)
}

fn run_batch_bounded(config: &Config) -> Result<Measurement, String> {
    let (mut producer_queue, mut consumer_queue) = RingBuffer::<u64>::new(config.capacity);
    let state = Arc::new(StartState::new());
    let messages = config.messages;
    let batch_size = config.batch_size;

    let mut producer_values = vec![0_u64; batch_size];
    let backoff_kind = config.backoff;
    let producer = spawn_worker(config.producer_core, Arc::clone(&state), move || {
        let mut result = ProducerResult {
            full_retries: 0,
            send_ok: true,
        };
        let mut backoff = Backoff::new(backoff_kind);
        let mut sent = 0_u64;
        while sent < messages {
            let count = batch_size.min((messages - sent) as usize);
            for (offset, value) in producer_values[..count].iter_mut().enumerate() {
                *value = sent + offset as u64;
            }
            let mut offset = 0;
            while offset < count {
                let (published, _) =
                    producer_queue.push_partial_slice(&producer_values[offset..count]);
                if published.is_empty() {
                    result.full_retries += 1;
                    backoff.wait();
                    continue;
                }
                offset += published.len();
                sent += published.len() as u64;
                backoff.reset();
            }
        }
        result
    });

    let mut consumer_values = vec![0_u64; batch_size];
    let backoff_kind = config.backoff;
    let consumer = spawn_worker(config.consumer_core, Arc::clone(&state), move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        let mut backoff = Backoff::new(backoff_kind);
        while result.received < messages {
            let target = batch_size.min((messages - result.received) as usize);
            let (received, _) = consumer_queue.pop_partial_slice(&mut consumer_values[..target]);
            if received.is_empty() {
                result.empty_retries += 1;
                backoff.wait();
                continue;
            }
            let first_expected = result.received;
            let mut batch_checksum = 0_u64;
            for &value in received.iter() {
                batch_checksum = batch_checksum.wrapping_add(value);
            }
            let mut order_mismatch = 0_u64;
            for (offset, &value) in received.iter().enumerate() {
                order_mismatch |= value ^ (first_expected + offset as u64);
            }
            result.fifo_ok &= order_mismatch == 0;
            result.checksum = result.checksum.wrapping_add(batch_checksum);
            result.received += received.len() as u64;
            backoff.reset();
        }
        result
    });

    while state.ready.load(Ordering::Acquire) != 2 {
        thread::yield_now();
    }
    let begin = Instant::now();
    state.start.store(true, Ordering::Release);
    finish_measurement(config, begin, producer, consumer)
}

fn run_batch_unbounded(config: &Config) -> Result<Measurement, String> {
    let (producer_queue, consumer_queue) = unbounded_spsc::channel::<u64>();
    let state = Arc::new(StartState::new());
    let messages = config.messages;
    let batch_size = config.batch_size;

    let mut producer_values = vec![0_u64; batch_size];
    let producer_finish_state = Arc::clone(&state);
    let producer = spawn_worker(config.producer_core, Arc::clone(&state), move || {
        let mut sent = 0_u64;
        while sent < messages {
            let count = batch_size.min((messages - sent) as usize);
            for (offset, value) in producer_values[..count].iter_mut().enumerate() {
                *value = sent + offset as u64;
            }
            for &value in &producer_values[..count] {
                if producer_queue.send(value).is_err() {
                    producer_finish_state
                        .producer_finished
                        .store(true, Ordering::Release);
                    return ProducerResult {
                        full_retries: 0,
                        send_ok: false,
                    };
                }
            }
            sent += count as u64;
        }
        producer_finish_state
            .producer_finished
            .store(true, Ordering::Release);
        while !producer_finish_state.consumer_done.load(Ordering::Acquire) {
            thread::yield_now();
        }
        ProducerResult {
            full_retries: 0,
            send_ok: true,
        }
    });

    let mut consumer_values = vec![0_u64; batch_size];
    let backoff_kind = config.backoff;
    let consumer_finish_state = Arc::clone(&state);
    let consumer = spawn_worker(config.consumer_core, Arc::clone(&state), move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        let mut backoff = Backoff::new(backoff_kind);
        let mut stalled_since: Option<Instant> = None;
        while result.received < messages {
            let target = batch_size.min((messages - result.received) as usize);
            let mut batch_received = 0_usize;
            while batch_received < target {
                match consumer_queue.try_recv() {
                    Ok(value) => {
                        stalled_since = None;
                        consumer_values[batch_received] = value;
                        batch_received += 1;
                    }
                    Err(unbounded_spsc::TryRecvError::Empty)
                    | Err(unbounded_spsc::TryRecvError::Disconnected) => {
                        if consumer_finish_state
                            .producer_finished
                            .load(Ordering::Acquire)
                        {
                            let since = *stalled_since.get_or_insert_with(Instant::now);
                            if since.elapsed() >= std::time::Duration::from_secs(1) {
                                result.fifo_ok = false;
                                consumer_finish_state
                                    .consumer_done
                                    .store(true, Ordering::Release);
                                return result;
                            }
                        }
                        break;
                    }
                }
            }
            if batch_received == 0 {
                result.empty_retries += 1;
                backoff.wait();
                continue;
            }
            let first_expected = result.received;
            let mut batch_checksum = 0_u64;
            for &value in &consumer_values[..batch_received] {
                batch_checksum = batch_checksum.wrapping_add(value);
            }
            let mut order_mismatch = 0_u64;
            for (offset, &value) in consumer_values[..batch_received].iter().enumerate() {
                order_mismatch |= value ^ (first_expected + offset as u64);
            }
            result.fifo_ok &= order_mismatch == 0;
            result.checksum = result.checksum.wrapping_add(batch_checksum);
            result.received += batch_received as u64;
            backoff.reset();
        }
        consumer_finish_state
            .consumer_done
            .store(true, Ordering::Release);
        result
    });

    while state.ready.load(Ordering::Acquire) != 2 {
        thread::yield_now();
    }
    let begin = Instant::now();
    state.start.store(true, Ordering::Release);
    finish_measurement(config, begin, producer, consumer)
}

fn run() -> Result<bool, String> {
    let config = parse_arguments()?;
    let measurement = match config.kind {
        CaseKind::RawBounded => run_bounded(&config)?,
        CaseKind::BatchBounded => run_batch_bounded(&config)?,
        CaseKind::Unbounded => run_unbounded(&config)?,
        CaseKind::BatchUnbounded => run_batch_unbounded(&config)?,
    };
    let valid = measurement.elapsed_ns > 0
        && measurement.send_ok
        && measurement.received == config.messages
        && measurement.fifo_ok
        && measurement.checksum == measurement.expected_checksum;
    let reported_capacity = if config.kind.is_bounded() {
        config.capacity
    } else {
        0
    };
    let reported_batch_size = if config.kind.is_batch() {
        config.batch_size
    } else {
        1
    };
    println!(
        "{{\"schema\":\"galay.spsc.paired.v4\",\"language\":\"rust\",\"case\":\"{}\",\"implementation\":\"{}\",\"api_profile\":\"{}\",\"comparison_scope\":\"{}\",\"topology\":\"1p1c\",\"payload_bytes\":8,\"capacity\":{},\"batch_size\":{},\"messages\":{},\"elapsed_ns\":{},\"messages_per_second\":{},\"received\":{},\"checksum\":{},\"expected_checksum\":{},\"fifo_ok\":{},\"full_retries\":{},\"empty_retries\":{},\"producer_placement\":\"{}\",\"consumer_placement\":\"{}\",\"backoff\":\"{}\",\"generator\":\"monotonic_u64\",\"valid\":{}}}",
        config.kind.name(),
        config.kind.implementation(),
        config.kind.api_profile(),
        config.kind.comparison_scope(),
        reported_capacity,
        reported_batch_size,
        config.messages,
        measurement.elapsed_ns,
        measurement.messages_per_second,
        measurement.received,
        measurement.checksum,
        measurement.expected_checksum,
        measurement.fifo_ok,
        measurement.full_retries,
        measurement.empty_retries,
        measurement.producer_placement.name(),
        measurement.consumer_placement.name(),
        config.backoff.name(),
        valid,
    );
    Ok(valid)
}

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::from(1),
        Err(error) => {
            eprintln!("spsc paired benchmark failed: {error}");
            ExitCode::from(2)
        }
    }
}
