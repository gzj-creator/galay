use std::process::ExitCode;
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::Instant;

struct Config {
    messages: u64,
    producers: usize,
    consumers: usize,
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
        fn thread_policy_set(thread: u32, flavor: i32, policy: *mut i32, count: u32) -> i32;
        fn pthread_set_qos_class_self_np(qos_class: u32, priority: i32) -> i32;
    }
    const THREAD_AFFINITY_POLICY: i32 = 4;
    const QOS_CLASS_USER_INTERACTIVE: u32 = 0x21;
    let online = thread::available_parallelism().map_or(1, |count| count.get());
    let mut policy = [((core_index % online) + 1) as i32];
    unsafe {
        let mach_thread = pthread_mach_thread_np(pthread_self());
        let _affinity = thread_policy_set(
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
        fn pthread_setaffinity_np(thread: usize, size: usize, cpuset: *const u8) -> i32;
    }
    let online = thread::available_parallelism().map_or(1, |count| count.get());
    let target = core_index % online;
    let mut cpuset = [0_u8; 128];
    if target >= cpuset.len() * 8 {
        return ThreadPlacement::Unsupported;
    }
    cpuset[target / 8] |= 1_u8 << (target % 8);
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
    let args = std::env::args().skip(1).collect::<Vec<_>>();
    if args.len() % 2 != 0 {
        return Err("missing option value".to_owned());
    }
    let mut config = Config {
        messages: 5_000_000,
        producers: 2,
        consumers: 2,
    };
    for pair in args.chunks_exact(2) {
        match pair[0].as_str() {
            "--messages" => {
                config.messages = pair[1]
                    .parse::<u64>()
                    .map_err(|_| "invalid message count".to_owned())?;
            }
            "--producers" => {
                config.producers = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid producer count".to_owned())?;
            }
            "--consumers" => {
                config.consumers = pair[1]
                    .parse::<usize>()
                    .map_err(|_| "invalid consumer count".to_owned())?;
            }
            _ => return Err(format!("unknown option: {}", pair[0])),
        }
    }
    if config.messages == 0 || config.messages > (1_u64 << 32) {
        return Err("message count must be in 1..=2^32".to_owned());
    }
    if config.producers != config.consumers || (config.producers != 2 && config.producers != 4) {
        return Err("producer and consumer counts must match and be 2 or 4".to_owned());
    }
    Ok(config)
}

fn expected_checksum(messages: u64) -> u64 {
    if messages & 1 == 0 {
        (messages / 2) * (messages - 1)
    } else {
        messages * ((messages - 1) / 2)
    }
}

fn placement_valid(placement: ThreadPlacement) -> bool {
    #[cfg(target_os = "macos")]
    return placement == ThreadPlacement::PerformanceClassOnly;
    #[cfg(target_os = "linux")]
    return placement == ThreadPlacement::PinnedToCore;
    #[cfg(not(any(target_os = "macos", target_os = "linux")))]
    return true;
}

fn run(config: &Config) -> Result<(u64, u64, u64, u64, ThreadPlacement), String> {
    let (sender, receiver) = crossbeam_channel::unbounded::<u64>();
    let ready = Arc::new(Barrier::new(config.producers + config.consumers + 1));
    let start = Arc::new(Barrier::new(config.producers + config.consumers + 1));
    let mut producers = Vec::with_capacity(config.producers);
    let mut consumers = Vec::with_capacity(config.consumers);

    for producer in 0..config.producers {
        let tx = sender.clone();
        let ready_gate = Arc::clone(&ready);
        let start_gate = Arc::clone(&start);
        let messages = config.messages;
        let producer_count = config.producers;
        producers.push(thread::spawn(move || -> Result<ThreadPlacement, String> {
            let placement = pin_current_thread(producer);
            ready_gate.wait();
            start_gate.wait();
            let first = messages * producer as u64 / producer_count as u64;
            let last = messages * (producer + 1) as u64 / producer_count as u64;
            for id in first..last {
                tx.send(id)
                    .map_err(|_| "crossbeam receiver disconnected".to_owned())?;
            }
            Ok(placement)
        }));
    }
    drop(sender);

    for consumer in 0..config.consumers {
        let rx = receiver.clone();
        let ready_gate = Arc::clone(&ready);
        let start_gate = Arc::clone(&start);
        let core = config.producers + consumer;
        consumers.push(thread::spawn(move || {
            let placement = pin_current_thread(core);
            ready_gate.wait();
            start_gate.wait();
            let mut received = 0_u64;
            let mut checksum = 0_u64;
            let mut empty_retries = 0_u64;
            loop {
                match rx.try_recv() {
                    Ok(value) => {
                        received += 1;
                        checksum = checksum.wrapping_add(value);
                    }
                    Err(crossbeam_channel::TryRecvError::Empty) => {
                        empty_retries += 1;
                        thread::yield_now();
                    }
                    Err(crossbeam_channel::TryRecvError::Disconnected) => break,
                }
            }
            (received, checksum, empty_retries, placement)
        }));
    }
    drop(receiver);

    ready.wait();
    let begin = Instant::now();
    start.wait();
    let mut placement = ThreadPlacement::PinnedToCore;
    for producer in producers {
        let current = producer
            .join()
            .map_err(|_| "crossbeam producer panicked".to_owned())??;
        placement = placement.max(current);
    }
    let mut received = 0_u64;
    let mut checksum = 0_u64;
    let mut empty_retries = 0_u64;
    for consumer in consumers {
        let (local_received, local_checksum, local_retries, current) = consumer
            .join()
            .map_err(|_| "crossbeam consumer panicked".to_owned())?;
        received += local_received;
        checksum = checksum.wrapping_add(local_checksum);
        empty_retries += local_retries;
        placement = placement.max(current);
    }
    Ok((
        begin.elapsed().as_nanos() as u64,
        received,
        checksum,
        empty_retries,
        placement,
    ))
}

fn benchmark() -> Result<(), String> {
    let config = parse_arguments()?;
    let (elapsed_ns, received, checksum, empty_retries, placement) = run(&config)?;
    let expected = expected_checksum(config.messages);
    let valid = elapsed_ns > 0
        && received == config.messages
        && checksum == expected
        && placement_valid(placement);
    let messages_per_second = config.messages as f64 * 1_000_000_000.0 / elapsed_ns as f64;
    println!(
        "{{\"schema\":\"galay.mpmc.paired.v1\",\"language\":\"rust\",\"case\":\"unbounded\",\"path\":\"token\",\"topology\":\"{}p{}c\",\"payload_bytes\":8,\"messages\":{},\"elapsed_ns\":{},\"messages_per_second\":{},\"received\":{},\"checksum\":{},\"expected_checksum\":{},\"send_retries\":0,\"empty_retries\":{},\"placement\":\"{}\",\"backoff\":\"yield\",\"generator\":\"partitioned_monotonic_u64\",\"valid\":{}}}",
        config.producers,
        config.consumers,
        config.messages,
        elapsed_ns,
        messages_per_second,
        received,
        checksum,
        expected,
        empty_retries,
        placement.name(),
        valid,
    );
    if valid {
        Ok(())
    } else {
        Err("measurement validation failed".to_owned())
    }
}

fn main() -> ExitCode {
    match benchmark() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("mpmc paired benchmark failed: {error}");
            ExitCode::FAILURE
        }
    }
}
