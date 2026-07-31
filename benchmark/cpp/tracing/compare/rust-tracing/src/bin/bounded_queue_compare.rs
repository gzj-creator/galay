use crossbeam_channel::{bounded, TryRecvError, TrySendError};
use crossbeam_queue::ArrayQueue;
use std::sync::{atomic::{AtomicBool, Ordering}, Arc, Barrier};
use std::thread;
use std::time::Instant;

const TOTAL_MESSAGES: usize = 1_000_000;
const WARMUP_SAMPLES: usize = 1;
const SAMPLES: usize = 5;

type ThroughputFn = fn(usize, usize, usize, usize) -> (f64, usize);

fn run_array_queue(
    producer_count: usize,
    consumer_count: usize,
    total_messages: usize,
    capacity: usize,
) -> (f64, usize) {
    let queue = Arc::new(ArrayQueue::new(capacity));
    let producer_done = Arc::new(AtomicBool::new(false));
    let start_barrier = Arc::new(Barrier::new(producer_count + consumer_count + 1));
    let per_producer = total_messages / producer_count;

    let mut producers = Vec::with_capacity(producer_count);
    for producer in 0..producer_count {
        let queue = Arc::clone(&queue);
        let start_barrier = Arc::clone(&start_barrier);
        producers.push(thread::spawn(move || {
            start_barrier.wait();
            let first = producer * per_producer;
            for value in first..first + per_producer {
                let mut pending = value as i64;
                loop {
                    match queue.push(pending) {
                        Ok(()) => break,
                        Err(value) => {
                            pending = value;
                            thread::yield_now();
                        }
                    }
                }
            }
        }));
    }

    let mut consumers = Vec::with_capacity(consumer_count);
    for _ in 0..consumer_count {
        let queue = Arc::clone(&queue);
        let producer_done = Arc::clone(&producer_done);
        let start_barrier = Arc::clone(&start_barrier);
        consumers.push(thread::spawn(move || {
            start_barrier.wait();
            let mut received = 0usize;
            loop {
                if queue.pop().is_some() {
                    received += 1;
                } else if producer_done.load(Ordering::Acquire) {
                    break;
                } else {
                    thread::yield_now();
                }
            }
            received
        }));
    }

    let start = Instant::now();
    start_barrier.wait();
    for producer in producers {
        producer.join().expect("producer thread panicked");
    }
    producer_done.store(true, Ordering::Release);

    let mut received = 0usize;
    for consumer in consumers {
        received += consumer.join().expect("consumer thread panicked");
    }
    let elapsed = start.elapsed().as_secs_f64();
    (total_messages as f64 / elapsed, received)
}

fn run_crossbeam_channel(
    producer_count: usize,
    consumer_count: usize,
    total_messages: usize,
    capacity: usize,
) -> (f64, usize) {
    let (sender, receiver) = bounded::<i64>(capacity);
    let producer_done = Arc::new(AtomicBool::new(false));
    let start_barrier = Arc::new(Barrier::new(producer_count + consumer_count + 1));
    let per_producer = total_messages / producer_count;

    let mut producers = Vec::with_capacity(producer_count);
    for producer in 0..producer_count {
        let sender = sender.clone();
        let start_barrier = Arc::clone(&start_barrier);
        producers.push(thread::spawn(move || {
            start_barrier.wait();
            let first = producer * per_producer;
            for value in first..first + per_producer {
                let mut pending = value as i64;
                loop {
                    match sender.try_send(pending) {
                        Ok(()) => break,
                        Err(TrySendError::Full(value)) => {
                            pending = value;
                            thread::yield_now();
                        }
                        Err(TrySendError::Disconnected(_)) => return,
                    }
                }
            }
        }));
    }
    drop(sender);

    let mut consumers = Vec::with_capacity(consumer_count);
    for _ in 0..consumer_count {
        let receiver = receiver.clone();
        let producer_done = Arc::clone(&producer_done);
        let start_barrier = Arc::clone(&start_barrier);
        consumers.push(thread::spawn(move || {
            start_barrier.wait();
            let mut received = 0usize;
            loop {
                match receiver.try_recv() {
                    Ok(_) => received += 1,
                    Err(TryRecvError::Empty) if !producer_done.load(Ordering::Acquire) => {
                        thread::yield_now();
                    }
                    Err(TryRecvError::Empty | TryRecvError::Disconnected) => break,
                }
            }
            received
        }));
    }
    drop(receiver);

    let start = Instant::now();
    start_barrier.wait();
    for producer in producers {
        producer.join().expect("producer thread panicked");
    }
    producer_done.store(true, Ordering::Release);

    let mut received = 0usize;
    for consumer in consumers {
        received += consumer.join().expect("consumer thread panicked");
    }
    let elapsed = start.elapsed().as_secs_f64();
    (total_messages as f64 / elapsed, received)
}

fn print_samples(
    name: &str,
    benchmark: ThroughputFn,
    producer_count: usize,
    consumer_count: usize,
    capacity: usize,
) {
    for _ in 0..WARMUP_SAMPLES {
        let (_, warmup_received) = benchmark(
            producer_count,
            consumer_count,
            TOTAL_MESSAGES,
            capacity,
        );
        assert_eq!(warmup_received, TOTAL_MESSAGES, "warmup lost messages");
    }

    let mut samples = Vec::with_capacity(SAMPLES);
    let mut received = 0usize;
    for _ in 0..SAMPLES {
        let (messages_per_second, current_received) =
            benchmark(producer_count, consumer_count, TOTAL_MESSAGES, capacity);
        samples.push(messages_per_second);
        received = current_received;
    }
    samples.sort_by(|left, right| left.partial_cmp(right).expect("finite throughput"));
    println!(
        "queue={name} producers={producer_count} consumers={consumer_count} capacity={capacity} median_msg_s={:.2} received={received}",
        samples[SAMPLES / 2]
    );
}

fn main() {
    for &(producer_count, consumer_count, capacity) in &[
        (1, 1, 256),
        (1, 1, 4096),
        (4, 1, 256),
        (1, 4, 256),
        (4, 4, 256),
        (4, 4, 4096),
    ] {
        print_samples(
            "crossbeam_array_queue",
            run_array_queue,
            producer_count,
            consumer_count,
            capacity,
        );
        print_samples(
            "crossbeam_channel_bounded",
            run_crossbeam_channel,
            producer_count,
            consumer_count,
            capacity,
        );
    }
}
