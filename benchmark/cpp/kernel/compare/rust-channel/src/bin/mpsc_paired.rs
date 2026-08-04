use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

#[derive(Clone, Copy, PartialEq, Eq)]
enum CaseKind {
    Bounded,
    Unbounded,
}

impl CaseKind {
    fn name(self) -> &'static str {
        match self {
            Self::Bounded => "bounded",
            Self::Unbounded => "unbounded",
        }
    }
}

struct Config {
    messages: u64,
    capacity: usize,
    producers: usize,
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
    producers_done: AtomicUsize,
    expected_workers: usize,
    start: AtomicBool,
    failed: AtomicBool,
}

impl StartState {
    fn new(expected_workers: usize) -> Self {
        Self {
            ready: AtomicUsize::new(0),
            producers_done: AtomicUsize::new(0),
            expected_workers,
            start: AtomicBool::new(false),
            failed: AtomicBool::new(false),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum ThreadPlacement {
    PinnedToCore,
    PerformanceClassOnly,
    Unsupported,
}

impl ThreadPlacement {
    fn name(self) -> &'static str {
        match self {
            Self::PinnedToCore => "pinned",
            Self::PerformanceClassOnly => "perf-class-only",
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
    // contains one integer for THREAD_AFFINITY_POLICY_COUNT. Darwin's
    // affinity policy is only a scheduler hint, never evidence of core pinning.
    unsafe {
        let mach_thread = pthread_mach_thread_np(pthread_self());
        let _affinity_hint_result = thread_policy_set(
            mach_thread,
            THREAD_AFFINITY_POLICY,
            policy.as_mut_ptr(),
            policy.len() as u32,
        );
        if pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0 {
            return ThreadPlacement::PerformanceClassOnly;
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
        producers: 2,
        producer_core: 0,
        consumer_core: 2,
        kind: CaseKind::Bounded,
    };
    for pair in arguments.chunks_exact(2) {
        match pair[0].as_str() {
            "--case" => {
                config.kind = match pair[1].as_str() {
                    "bounded" => CaseKind::Bounded,
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
            "--producers" => {
                config.producers = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid producer count".to_owned())?;
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
    if config.producers < 2 || config.producers > 32 {
        return Err("producer count must be between 2 and 32".to_owned());
    }
    if config.kind == CaseKind::Bounded
        && (config.capacity < 2 || !config.capacity.is_power_of_two())
    {
        return Err("bounded capacity must be a power of two and at least 2".to_owned());
    }
    let producer_end = config
        .producer_core
        .checked_add(config.producers)
        .ok_or_else(|| "producer core range overflows".to_owned())?;
    if config.consumer_core >= config.producer_core && config.consumer_core < producer_end {
        return Err("consumer core overlaps the producer core range".to_owned());
    }
    Ok(config)
}

fn first_sequence(config: &Config, producer: usize) -> u64 {
    config.messages * producer as u64 / config.producers as u64
}

fn producer_messages(config: &Config, producer: usize) -> u64 {
    first_sequence(config, producer + 1) - first_sequence(config, producer)
}

fn triangular_checksum(count: u64) -> u64 {
    if count == 0 {
        return 0;
    }
    if count & 1 == 0 {
        (count / 2).wrapping_mul(count - 1)
    } else {
        count.wrapping_mul((count - 1) / 2)
    }
}

fn expected_checksum(config: &Config) -> u64 {
    let mut checksum = 0_u64;
    for producer in 0..config.producers {
        let count = producer_messages(config, producer);
        checksum = checksum.wrapping_add(
            ((producer as u64) << 32)
                .wrapping_mul(count)
                .wrapping_add(triangular_checksum(count)),
        );
    }
    checksum
}

fn encode_value(producer: usize, sequence: u64) -> u64 {
    ((producer as u64) << 32) | sequence
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
        let ready = state
            .ready
            .fetch_add(1, Ordering::Release)
            .saturating_add(1);
        if ready > state.expected_workers {
            state.failed.store(true, Ordering::Release);
        }
        while !state.start.load(Ordering::Acquire) {
            thread::yield_now();
        }
        (placement, work())
    })
}

fn finish_measurement(
    config: &Config,
    state: &StartState,
    begin: Instant,
    producers: Vec<thread::JoinHandle<(ThreadPlacement, ProducerResult)>>,
    consumer: thread::JoinHandle<(ThreadPlacement, ConsumerResult)>,
) -> Result<Measurement, String> {
    let mut producer_placement = ThreadPlacement::PinnedToCore;
    let mut full_retries = 0_u64;
    let mut send_ok = true;
    for producer in producers {
        let (placement, result) = producer
            .join()
            .map_err(|_| "producer thread panicked".to_owned())?;
        producer_placement = producer_placement.max(placement);
        full_retries = full_retries.wrapping_add(result.full_retries);
        send_ok &= result.send_ok;
    }
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
        expected_checksum: expected_checksum(config),
        full_retries,
        empty_retries: consumer_result.empty_retries,
        producer_placement,
        consumer_placement,
        fifo_ok: consumer_result.fifo_ok,
        send_ok: send_ok && !state.failed.load(Ordering::Acquire),
    })
}

fn run_bounded(config: &Config) -> Result<Measurement, String> {
    let (sender, receiver) = crossbeam_channel::bounded::<u64>(config.capacity);
    let producer_count = config.producers;
    let state = Arc::new(StartState::new(producer_count + 1));
    let mut producers = Vec::with_capacity(producer_count);

    for producer in 0..producer_count {
        let producer_sender = sender.clone();
        let producer_state = Arc::clone(&state);
        let worker_state = Arc::clone(&state);
        let count = producer_messages(config, producer);
        producers.push(spawn_worker(
            config.producer_core + producer,
            worker_state,
            move || {
                let mut full_retries = 0_u64;
                let mut send_ok = true;
                'messages: for sequence in 0..count {
                    let mut pending = encode_value(producer, sequence);
                    loop {
                        match producer_sender.try_send(pending) {
                            Ok(()) => break,
                            Err(crossbeam_channel::TrySendError::Full(returned)) => {
                                pending = returned;
                                full_retries = full_retries.wrapping_add(1);
                                thread::yield_now();
                            }
                            Err(crossbeam_channel::TrySendError::Disconnected(_returned)) => {
                                send_ok = false;
                                producer_state.failed.store(true, Ordering::Release);
                                break 'messages;
                            }
                        }
                    }
                }
                let done = producer_state
                    .producers_done
                    .fetch_add(1, Ordering::Release)
                    .saturating_add(1);
                if done > producer_count {
                    send_ok = false;
                    producer_state.failed.store(true, Ordering::Release);
                }
                ProducerResult {
                    full_retries,
                    send_ok,
                }
            },
        ));
    }
    drop(sender);

    let consumer_state = Arc::clone(&state);
    let worker_state = Arc::clone(&state);
    let messages = config.messages;
    let expected_counts = (0..producer_count)
        .map(|producer| producer_messages(config, producer))
        .collect::<Vec<_>>();
    let consumer = spawn_worker(config.consumer_core, worker_state, move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        let mut expected = vec![0_u64; producer_count];
        let mut all_producers_done = false;
        while result.received < messages && !consumer_state.failed.load(Ordering::Acquire) {
            match receiver.try_recv() {
                Ok(value) => {
                    let producer = (value >> 32) as usize;
                    let sequence = value & 0xffff_ffff;
                    if producer >= producer_count || sequence != expected[producer] {
                        result.fifo_ok = false;
                        consumer_state.failed.store(true, Ordering::Release);
                        break;
                    }
                    expected[producer] += 1;
                    result.checksum = result.checksum.wrapping_add(value);
                    result.received += 1;
                }
                Err(crossbeam_channel::TryRecvError::Empty) => {
                    result.empty_retries = result.empty_retries.wrapping_add(1);
                    if all_producers_done {
                        break;
                    }
                    all_producers_done =
                        consumer_state.producers_done.load(Ordering::Acquire) == producer_count;
                    if !all_producers_done {
                        thread::yield_now();
                    }
                }
                Err(crossbeam_channel::TryRecvError::Disconnected) => break,
            }
        }
        for producer in 0..producer_count {
            result.fifo_ok &= expected[producer] == expected_counts[producer];
        }
        result
    });

    while state.ready.load(Ordering::Acquire) < state.expected_workers {
        thread::yield_now();
    }
    let begin = Instant::now();
    state.start.store(true, Ordering::Release);
    finish_measurement(config, &state, begin, producers, consumer)
}

fn run_unbounded(config: &Config) -> Result<Measurement, String> {
    let (sender, receiver) = crossbeam_channel::unbounded::<u64>();
    let producer_count = config.producers;
    let state = Arc::new(StartState::new(producer_count + 1));
    let mut producers = Vec::with_capacity(producer_count);

    for producer in 0..producer_count {
        let producer_sender = sender.clone();
        let producer_state = Arc::clone(&state);
        let worker_state = Arc::clone(&state);
        let count = producer_messages(config, producer);
        producers.push(spawn_worker(
            config.producer_core + producer,
            worker_state,
            move || {
                let mut send_ok = true;
                for sequence in 0..count {
                    if producer_sender
                        .send(encode_value(producer, sequence))
                        .is_err()
                    {
                        send_ok = false;
                        producer_state.failed.store(true, Ordering::Release);
                        break;
                    }
                }
                let done = producer_state
                    .producers_done
                    .fetch_add(1, Ordering::Release)
                    .saturating_add(1);
                if done > producer_count {
                    send_ok = false;
                    producer_state.failed.store(true, Ordering::Release);
                }
                ProducerResult {
                    full_retries: 0,
                    send_ok,
                }
            },
        ));
    }
    drop(sender);

    let consumer_state = Arc::clone(&state);
    let worker_state = Arc::clone(&state);
    let messages = config.messages;
    let expected_counts = (0..producer_count)
        .map(|producer| producer_messages(config, producer))
        .collect::<Vec<_>>();
    let consumer = spawn_worker(config.consumer_core, worker_state, move || {
        let mut result = ConsumerResult {
            received: 0,
            checksum: 0,
            empty_retries: 0,
            fifo_ok: true,
        };
        let mut expected = vec![0_u64; producer_count];
        let mut all_producers_done = false;
        while result.received < messages && !consumer_state.failed.load(Ordering::Acquire) {
            match receiver.try_recv() {
                Ok(value) => {
                    let producer = (value >> 32) as usize;
                    let sequence = value & 0xffff_ffff;
                    if producer >= producer_count || sequence != expected[producer] {
                        result.fifo_ok = false;
                        consumer_state.failed.store(true, Ordering::Release);
                        break;
                    }
                    expected[producer] += 1;
                    result.checksum = result.checksum.wrapping_add(value);
                    result.received += 1;
                }
                Err(crossbeam_channel::TryRecvError::Empty) => {
                    result.empty_retries = result.empty_retries.wrapping_add(1);
                    if all_producers_done {
                        break;
                    }
                    all_producers_done =
                        consumer_state.producers_done.load(Ordering::Acquire) == producer_count;
                    if !all_producers_done {
                        thread::yield_now();
                    }
                }
                Err(crossbeam_channel::TryRecvError::Disconnected) => break,
            }
        }
        for producer in 0..producer_count {
            result.fifo_ok &= expected[producer] == expected_counts[producer];
        }
        result
    });

    while state.ready.load(Ordering::Acquire) < state.expected_workers {
        thread::yield_now();
    }
    let begin = Instant::now();
    state.start.store(true, Ordering::Release);
    finish_measurement(config, &state, begin, producers, consumer)
}

fn run() -> Result<bool, String> {
    let config = parse_arguments()?;
    let measurement = match config.kind {
        CaseKind::Bounded => run_bounded(&config)?,
        CaseKind::Unbounded => run_unbounded(&config)?,
    };
    let valid = measurement.elapsed_ns > 0
        && measurement.send_ok
        && measurement.received == config.messages
        && measurement.fifo_ok
        && measurement.checksum == measurement.expected_checksum;
    let reported_capacity = if config.kind == CaseKind::Bounded {
        config.capacity
    } else {
        0
    };
    println!(
        "{{\"schema\":\"galay.mpsc.paired.v1\",\"language\":\"rust\",\"case\":\"{}\",\"topology\":\"{}p1c\",\"ordering\":\"producer_fifo\",\"payload_bytes\":8,\"capacity\":{},\"messages\":{},\"elapsed_ns\":{},\"messages_per_second\":{},\"received\":{},\"checksum\":{},\"expected_checksum\":{},\"fifo_ok\":{},\"full_retries\":{},\"empty_retries\":{},\"producer_placement\":\"{}\",\"consumer_placement\":\"{}\",\"backoff\":\"yield\",\"generator\":\"per_producer_monotonic_u64\",\"valid\":{}}}",
        config.kind.name(),
        config.producers,
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
            eprintln!("mpsc paired benchmark failed: {error}");
            ExitCode::from(2)
        }
    }
}
