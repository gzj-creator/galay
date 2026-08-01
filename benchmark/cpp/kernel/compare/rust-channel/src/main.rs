use crossbeam_queue::ArrayQueue;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::Instant;

const MESSAGES: i64 = 5_000_000;
const WARMUP_SAMPLES: usize = 1;
const SAMPLES: usize = 7;
const EXPECTED_SUM: i64 = (MESSAGES - 1) * MESSAGES / 2;

type BenchResult<T> = Result<T, String>;

#[derive(Clone, Copy)]
struct Measurement {
    messages_per_second: f64,
    received: i64,
    sum: i64,
    full_retries: u64,
    empty_retries: u64,
    placement: ThreadPlacement,
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(usize)]
enum ThreadPlacement {
    PinnedToCore = 0,
    PerformanceClassOnly = 1,
    Unsupported = 2,
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

#[derive(Clone)]
struct PlacementTracker(Arc<AtomicUsize>);

impl PlacementTracker {
    fn new() -> Self {
        Self(Arc::new(AtomicUsize::new(
            ThreadPlacement::PinnedToCore as usize,
        )))
    }

    fn place_current_thread(&self, core_index: usize) {
        self.0
            .fetch_max(pin_current_thread(core_index) as usize, Ordering::Relaxed);
    }

    fn placement(&self) -> ThreadPlacement {
        match self.0.load(Ordering::Relaxed) {
            value if value == ThreadPlacement::PinnedToCore as usize => {
                ThreadPlacement::PinnedToCore
            }
            value if value == ThreadPlacement::PerformanceClassOnly as usize => {
                ThreadPlacement::PerformanceClassOnly
            }
            _ => ThreadPlacement::Unsupported,
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

    // SAFETY: declarations match the Darwin pthread/Mach ABIs and policy
    // points to one integer for THREAD_AFFINITY_POLICY_COUNT.
    unsafe {
        let mach_thread = pthread_mach_thread_np(pthread_self());
        let _ = thread_policy_set(
            mach_thread,
            THREAD_AFFINITY_POLICY,
            policy.as_mut_ptr(),
            policy.len() as u32,
        );
        // THREAD_AFFINITY_POLICY is only a scheduler hint on Darwin. QoS is
        // the enforceable condition shared with the C++ benchmark.
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

    // SAFETY: cpuset is a correctly sized, initialized Linux cpu_set_t bitmap
    // for the duration of pthread_setaffinity_np.
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

#[derive(Clone, Copy)]
enum CaseKind {
    ArrayQueue {
        producers: usize,
        consumers: usize,
        capacity: usize,
    },
    CrossbeamUnbounded {
        producers: usize,
        consumers: usize,
    },
}

impl CaseKind {
    fn run(self) -> BenchResult<Measurement> {
        match self {
            Self::ArrayQueue {
                producers,
                consumers,
                capacity,
            } => run_array_queue(producers, consumers, capacity),
            Self::CrossbeamUnbounded {
                producers,
                consumers,
            } => run_crossbeam_unbounded(producers, consumers),
        }
    }
}

#[derive(Clone, Copy)]
struct Case {
    name: &'static str,
    topology: &'static str,
    producers: usize,
    consumers: usize,
    capacity: Option<usize>,
    kind: CaseKind,
}

fn finish_measurement(
    begin: Instant,
    received: i64,
    sum: i64,
    full_retries: u64,
    empty_retries: u64,
    placement: ThreadPlacement,
) -> Measurement {
    let elapsed = begin.elapsed().as_secs_f64();
    Measurement {
        messages_per_second: if elapsed > 0.0 {
            MESSAGES as f64 / elapsed
        } else {
            0.0
        },
        received,
        sum,
        full_retries,
        empty_retries,
        placement,
    }
}

fn join_thread<T>(handle: thread::JoinHandle<T>, role: &str) -> BenchResult<T> {
    handle.join().map_err(|_| format!("{role} thread panicked"))
}

fn run_array_queue(
    producer_count: usize,
    consumer_count: usize,
    capacity: usize,
) -> BenchResult<Measurement> {
    let queue = Arc::new(ArrayQueue::<i64>::new(capacity));
    let done = Arc::new(AtomicBool::new(false));
    let participants = producer_count + consumer_count + 1;
    let ready = Arc::new(Barrier::new(participants));
    let start = Arc::new(Barrier::new(participants));
    let placement = PlacementTracker::new();
    let mut producers = Vec::with_capacity(producer_count);
    let mut consumers = Vec::with_capacity(consumer_count);

    for producer_id in 0..producer_count {
        let producer_queue = Arc::clone(&queue);
        let producer_ready = Arc::clone(&ready);
        let producer_start = Arc::clone(&start);
        let producer_placement = placement.clone();
        producers.push(thread::spawn(move || {
            producer_placement.place_current_thread(producer_id);
            producer_ready.wait();
            producer_start.wait();
            let first = MESSAGES * producer_id as i64 / producer_count as i64;
            let last = MESSAGES * (producer_id + 1) as i64 / producer_count as i64;
            let mut full_retries = 0_u64;
            for value in first..last {
                let mut pending = value;
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
            full_retries
        }));
    }

    for consumer_id in 0..consumer_count {
        let consumer_queue = Arc::clone(&queue);
        let consumer_done = Arc::clone(&done);
        let consumer_ready = Arc::clone(&ready);
        let consumer_start = Arc::clone(&start);
        let consumer_placement = placement.clone();
        consumers.push(thread::spawn(move || {
            consumer_placement.place_current_thread(producer_count + consumer_id);
            consumer_ready.wait();
            consumer_start.wait();
            let mut received = 0_i64;
            let mut sum = 0_i64;
            let mut empty_retries = 0_u64;
            loop {
                if let Some(value) = consumer_queue.pop() {
                    received += 1;
                    sum += value;
                    continue;
                }
                if consumer_done.load(Ordering::Acquire) {
                    break;
                }
                empty_retries += 1;
                thread::yield_now();
            }
            (received, sum, empty_retries)
        }));
    }

    ready.wait();
    let begin = Instant::now();
    start.wait();
    let mut full_retries = 0_u64;
    for producer in producers {
        full_retries += join_thread(producer, "ArrayQueue producer")?;
    }
    done.store(true, Ordering::Release);

    let mut received = 0_i64;
    let mut sum = 0_i64;
    let mut empty_retries = 0_u64;
    for consumer in consumers {
        let (local_received, local_sum, local_empty_retries) =
            join_thread(consumer, "ArrayQueue consumer")?;
        received += local_received;
        sum += local_sum;
        empty_retries += local_empty_retries;
    }
    Ok(finish_measurement(
        begin,
        received,
        sum,
        full_retries,
        empty_retries,
        placement.placement(),
    ))
}

fn run_crossbeam_unbounded(
    producer_count: usize,
    consumer_count: usize,
) -> BenchResult<Measurement> {
    let (sender, receiver) = crossbeam_channel::unbounded::<i64>();
    let participants = producer_count + consumer_count + 1;
    let ready = Arc::new(Barrier::new(participants));
    let start = Arc::new(Barrier::new(participants));
    let placement = PlacementTracker::new();
    let mut producers = Vec::with_capacity(producer_count);
    let mut consumers = Vec::with_capacity(consumer_count);

    for producer_id in 0..producer_count {
        let producer_sender = sender.clone();
        let producer_ready = Arc::clone(&ready);
        let producer_start = Arc::clone(&start);
        let producer_placement = placement.clone();
        producers.push(thread::spawn(move || -> BenchResult<()> {
            producer_placement.place_current_thread(producer_id);
            producer_ready.wait();
            producer_start.wait();
            let first = MESSAGES * producer_id as i64 / producer_count as i64;
            let last = MESSAGES * (producer_id + 1) as i64 / producer_count as i64;
            for value in first..last {
                producer_sender
                    .send(value)
                    .map_err(|_| "crossbeam receiver disconnected".to_owned())?;
            }
            Ok(())
        }));
    }
    drop(sender);

    for consumer_id in 0..consumer_count {
        let consumer_receiver = receiver.clone();
        let consumer_ready = Arc::clone(&ready);
        let consumer_start = Arc::clone(&start);
        let consumer_placement = placement.clone();
        consumers.push(thread::spawn(move || {
            consumer_placement.place_current_thread(producer_count + consumer_id);
            consumer_ready.wait();
            consumer_start.wait();
            let mut received = 0_i64;
            let mut sum = 0_i64;
            let mut empty_retries = 0_u64;
            loop {
                match consumer_receiver.try_recv() {
                    Ok(value) => {
                        received += 1;
                        sum += value;
                    }
                    Err(crossbeam_channel::TryRecvError::Empty) => {
                        empty_retries += 1;
                        thread::yield_now();
                    }
                    Err(crossbeam_channel::TryRecvError::Disconnected) => break,
                }
            }
            (received, sum, empty_retries)
        }));
    }
    drop(receiver);

    ready.wait();
    let begin = Instant::now();
    start.wait();
    for producer in producers {
        join_thread(producer, "crossbeam producer")??;
    }

    let mut received = 0_i64;
    let mut sum = 0_i64;
    let mut empty_retries = 0_u64;
    for consumer in consumers {
        let (local_received, local_sum, local_empty_retries) =
            join_thread(consumer, "crossbeam consumer")?;
        received += local_received;
        sum += local_sum;
        empty_retries += local_empty_retries;
    }
    Ok(finish_measurement(
        begin,
        received,
        sum,
        0,
        empty_retries,
        placement.placement(),
    ))
}

fn validate(case: &Case, measurement: &Measurement) -> BenchResult<()> {
    #[cfg(target_os = "macos")]
    let placement_valid = measurement.placement == ThreadPlacement::PerformanceClassOnly;
    #[cfg(target_os = "linux")]
    let placement_valid = measurement.placement == ThreadPlacement::PinnedToCore;
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    let placement_valid = true;

    if measurement.messages_per_second <= 0.0
        || !measurement.messages_per_second.is_finite()
        || measurement.received != MESSAGES
        || measurement.sum != EXPECTED_SUM
        || !placement_valid
    {
        return Err(format!(
            "{} {} validation failed: received={} sum={} msg_s={} placement={}",
            case.name,
            case.topology,
            measurement.received,
            measurement.sum,
            measurement.messages_per_second,
            measurement.placement.name()
        ));
    }
    Ok(())
}

fn median_f64(mut values: Vec<f64>) -> f64 {
    values.sort_by(f64::total_cmp);
    values[values.len() / 2]
}

fn median_u64(mut values: Vec<u64>) -> u64 {
    values.sort_unstable();
    values[values.len() / 2]
}

fn print_summary(case: &Case, samples: &[Measurement]) {
    let throughput = samples
        .iter()
        .map(|sample| sample.messages_per_second)
        .collect::<Vec<_>>();
    let minimum = throughput.iter().copied().fold(f64::INFINITY, f64::min);
    let maximum = throughput.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let full_retries = samples
        .iter()
        .map(|sample| sample.full_retries)
        .collect::<Vec<_>>();
    let empty_retries = samples
        .iter()
        .map(|sample| sample.empty_retries)
        .collect::<Vec<_>>();
    let placement = samples
        .iter()
        .map(|sample| sample.placement)
        .max()
        .unwrap_or(ThreadPlacement::Unsupported);

    print!(
        "{} topology={} producers={} consumers={}",
        case.name, case.topology, case.producers, case.consumers
    );
    match case.capacity {
        Some(capacity) => print!(" capacity={capacity}"),
        None => print!(" capacity=unbounded"),
    }
    println!(
        " messages={} samples={} median_msg_s={:.0} min_msg_s={:.0} max_msg_s={:.0} median_full_retries={} median_empty_retries={} placement={}",
        MESSAGES,
        samples.len(),
        median_f64(throughput),
        minimum,
        maximum,
        median_u64(full_retries),
        median_u64(empty_retries),
        placement.name()
    );
}

fn run_benchmark() -> BenchResult<()> {
    let cases = [
        Case {
            name: "crossbeam_array_queue",
            topology: "2p2c",
            producers: 2,
            consumers: 2,
            capacity: Some(256),
            kind: CaseKind::ArrayQueue {
                producers: 2,
                consumers: 2,
                capacity: 256,
            },
        },
        Case {
            name: "crossbeam_array_queue",
            topology: "4p4c",
            producers: 4,
            consumers: 4,
            capacity: Some(256),
            kind: CaseKind::ArrayQueue {
                producers: 4,
                consumers: 4,
                capacity: 256,
            },
        },
        Case {
            name: "crossbeam_array_queue",
            topology: "2p2c",
            producers: 2,
            consumers: 2,
            capacity: Some(4096),
            kind: CaseKind::ArrayQueue {
                producers: 2,
                consumers: 2,
                capacity: 4096,
            },
        },
        Case {
            name: "crossbeam_array_queue",
            topology: "4p4c",
            producers: 4,
            consumers: 4,
            capacity: Some(4096),
            kind: CaseKind::ArrayQueue {
                producers: 4,
                consumers: 4,
                capacity: 4096,
            },
        },
        Case {
            name: "crossbeam_channel_mpmc",
            topology: "2p2c",
            producers: 2,
            consumers: 2,
            capacity: None,
            kind: CaseKind::CrossbeamUnbounded {
                producers: 2,
                consumers: 2,
            },
        },
        Case {
            name: "crossbeam_channel_mpmc",
            topology: "4p4c",
            producers: 4,
            consumers: 4,
            capacity: None,
            kind: CaseKind::CrossbeamUnbounded {
                producers: 4,
                consumers: 4,
            },
        },
    ];

    for _ in 0..WARMUP_SAMPLES {
        for case in &cases {
            let measurement = case.kind.run()?;
            validate(case, &measurement)?;
        }
    }

    let mut samples = vec![Vec::<Measurement>::with_capacity(SAMPLES); cases.len()];
    for sample in 0..SAMPLES {
        for offset in 0..cases.len() {
            let index = (sample + offset) % cases.len();
            let measurement = cases[index].kind.run()?;
            validate(&cases[index], &measurement)?;
            samples[index].push(measurement);
        }
    }

    for (case, case_samples) in cases.iter().zip(samples.iter()) {
        print_summary(case, case_samples);
    }
    Ok(())
}

fn main() {
    if let Err(error) = run_benchmark() {
        eprintln!("rust channel benchmark failed: {error}");
        std::process::exit(1);
    }
}
