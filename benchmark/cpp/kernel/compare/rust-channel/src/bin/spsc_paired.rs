use crossbeam_queue::{ArrayQueue, SegQueue};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

#[derive(Clone, Copy, PartialEq, Eq)]
enum CaseKind {
    RawBounded,
    Unbounded,
}

impl CaseKind {
    fn name(self) -> &'static str {
        match self {
            Self::RawBounded => "raw_bounded",
            Self::Unbounded => "unbounded",
        }
    }
}

struct Config {
    messages: u64,
    capacity: usize,
    producer_core: usize,
    consumer_core: usize,
    kind: CaseKind,
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
}

impl StartState {
    fn new() -> Self {
        Self {
            ready: AtomicUsize::new(0),
            start: AtomicBool::new(false),
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
        producer_core: 0,
        consumer_core: 1,
        kind: CaseKind::RawBounded,
    };
    for pair in arguments.chunks_exact(2) {
        match pair[0].as_str() {
            "--case" => {
                config.kind = match pair[1].as_str() {
                    "raw_bounded" => CaseKind::RawBounded,
                    "unbounded" => CaseKind::Unbounded,
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
    if config.kind == CaseKind::RawBounded
        && (config.capacity < 2 || !config.capacity.is_power_of_two())
    {
        return Err("bounded capacity must be a power of two and at least 2".to_owned());
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
    let queue = Arc::new(ArrayQueue::<u64>::new(config.capacity));
    let state = Arc::new(StartState::new());

    let producer_queue = Arc::clone(&queue);
    let messages = config.messages;
    let producer = spawn_worker(config.producer_core, Arc::clone(&state), move || {
        let mut full_retries = 0_u64;
        for sequence in 0..messages {
            let mut pending = sequence;
            loop {
                match producer_queue.push(pending) {
                    Ok(()) => break,
                    Err(returned) => {
                        pending = returned;
                        full_retries += 1;
                        thread::yield_now();
                    }
                }
            }
        }
        ProducerResult {
            full_retries,
            send_ok: true,
        }
    });

    let consumer_queue = Arc::clone(&queue);
    let consumer = spawn_worker(config.consumer_core, Arc::clone(&state), move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        while result.received < messages {
            if let Some(value) = consumer_queue.pop() {
                result.fifo_ok &= value == result.received;
                result.checksum = result.checksum.wrapping_add(value);
                result.received += 1;
                continue;
            }
            result.empty_retries += 1;
            // Drain to the expected count so a transient empty pop after
            // producer completion cannot be reported as data loss.
            thread::yield_now();
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
    let queue = Arc::new(SegQueue::<u64>::new());
    let state = Arc::new(StartState::new());

    let producer_queue = Arc::clone(&queue);
    let messages = config.messages;
    let producer = spawn_worker(config.producer_core, Arc::clone(&state), move || {
        for sequence in 0..messages {
            producer_queue.push(sequence);
        }
        ProducerResult {
            full_retries: 0,
            send_ok: true,
        }
    });

    let consumer_queue = Arc::clone(&queue);
    let consumer = spawn_worker(config.consumer_core, Arc::clone(&state), move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        while result.received < messages {
            if let Some(value) = consumer_queue.pop() {
                result.fifo_ok &= value == result.received;
                result.checksum = result.checksum.wrapping_add(value);
                result.received += 1;
                continue;
            }
            result.empty_retries += 1;
            // Drain to the expected count so a transient empty pop after
            // producer completion cannot be reported as data loss.
            thread::yield_now();
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

fn run() -> Result<bool, String> {
    let config = parse_arguments()?;
    let measurement = match config.kind {
        CaseKind::RawBounded => run_bounded(&config)?,
        CaseKind::Unbounded => run_unbounded(&config)?,
    };
    let valid = measurement.elapsed_ns > 0
        && measurement.send_ok
        && measurement.received == config.messages
        && measurement.fifo_ok
        && measurement.checksum == measurement.expected_checksum;
    let reported_capacity = if config.kind == CaseKind::RawBounded {
        config.capacity
    } else {
        0
    };
    println!(
        "{{\"schema\":\"galay.spsc.paired.v1\",\"language\":\"rust\",\"case\":\"{}\",\"topology\":\"1p1c\",\"payload_bytes\":8,\"capacity\":{},\"messages\":{},\"elapsed_ns\":{},\"messages_per_second\":{},\"received\":{},\"checksum\":{},\"expected_checksum\":{},\"fifo_ok\":{},\"full_retries\":{},\"empty_retries\":{},\"producer_placement\":\"{}\",\"consumer_placement\":\"{}\",\"backoff\":\"yield\",\"generator\":\"monotonic_u64\",\"valid\":{}}}",
        config.kind.name(),
        reported_capacity,
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
