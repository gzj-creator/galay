/**
 * @file t172_mpsc_bounded_drain.cc
 * @brief 验证 bounded MPSC 无分配批量排空及 close/send-waiter 交互。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "result_writer.h"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <typename T>
concept HasDrainTo = requires(galay::mpsc::BoundedChannel<T>& channel,
                              std::vector<T>& destination) {
    { channel.drain_to(destination, size_t{1}) } noexcept -> std::same_as<size_t>;
};

static_assert(HasDrainTo<int>);

bool wait_for(const std::atomic<bool>& flag,
             std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

bool run_boundary_and_fifo()
{
    galay::mpsc::BoundedChannel<int> channel(8);
    std::vector<int> destination;
    destination.reserve(3);
    destination.push_back(-1);
    int* const storage = destination.data();

    if (channel.drain_to(destination, 4) != 0) {
        return false;
    }
    for (int value = 0; value < 5; ++value) {
        if (!channel.try_send(value)) {
            return false;
        }
    }
    if (channel.drain_to(destination, 0) != 0 || channel.size() != 5) {
        return false;
    }

    const size_t first = channel.drain_to(destination, 5);
    if (first != 2 || destination.data() != storage || destination.size() != 3 ||
        destination[0] != -1 || destination[1] != 0 || destination[2] != 1 ||
        channel.size() != 3) {
        return false;
    }

    destination.clear();
    const size_t second = channel.drain_to(destination, 2);
    auto last = channel.try_recv();
    return second == 2 && destination.data() == storage &&
        destination.size() == 2 && destination[0] == 2 && destination[1] == 3 &&
        last.has_value() && *last == 4 && channel.empty();
}

struct MoveGate
{
    std::atomic<bool> armed{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

struct CoordinatedValue
{
    CoordinatedValue(int input, MoveGate* inputGate) noexcept
        : gate(inputGate), value(input)
    {
    }

    CoordinatedValue(const CoordinatedValue&) = delete;
    CoordinatedValue& operator=(const CoordinatedValue&) = delete;

    CoordinatedValue(CoordinatedValue&& other) noexcept
        : gate(std::exchange(other.gate, nullptr)), value(other.value)
    {
        if (gate != nullptr && value == 1 &&
            gate->armed.load(std::memory_order_acquire)) {
            gate->entered.store(true, std::memory_order_release);
            while (!gate->release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }

    CoordinatedValue& operator=(CoordinatedValue&& other) noexcept
    {
        gate = std::exchange(other.gate, nullptr);
        value = other.value;
        return *this;
    }

    MoveGate* gate;
    int value;
};

bool run_concurrent_close()
{
    MoveGate gate;
    galay::mpsc::BoundedChannel<CoordinatedValue> channel(4);
    if (!channel.try_send(CoordinatedValue(1, &gate)) ||
        !channel.try_send(CoordinatedValue(2, &gate))) {
        return false;
    }

    std::vector<CoordinatedValue> destination;
    destination.reserve(2);
    size_t drained = 0;
    gate.armed.store(true, std::memory_order_release);
    std::thread consumer([&]() {
        drained = channel.drain_to(destination, 2);
    });

    const bool entered = wait_for(gate.entered);
    if (entered) {
        channel.close();
    }
    gate.release.store(true, std::memory_order_release);
    consumer.join();

    return entered && drained == 2 && channel.is_closed() && channel.empty() &&
        destination.size() == 2 && destination[0].value == 1 &&
        destination[1].value == 2;
}

struct SendState
{
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    bool success = false;
};

galay::kernel::Task<void> send_one(
    galay::mpsc::BoundedChannel<int>* channel, SendState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->send(3);
    state->success = result.has_value();
    state->done.store(true, std::memory_order_release);
    co_return;
}

bool run_send_waiter_progress()
{
    galay::mpsc::BoundedChannel<int> channel(2);
    if (!channel.try_send(1) || !channel.try_send(2)) {
        return false;
    }

    galay::kernel::ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }
    SendState state;
    if (!galay::kernel::schedule_task(scheduler, send_one(&channel, &state)) ||
        !wait_for(state.entered)) {
        scheduler.stop();
        return false;
    }

    std::vector<int> first;
    first.reserve(1);
    const size_t drained = channel.drain_to(first, 1);
    const bool done = wait_for(state.done);
    scheduler.stop();
    if (drained != 1 || first.size() != 1 || first[0] != 1 || !done ||
        !state.success) {
        return false;
    }

    std::vector<int> rest;
    rest.reserve(2);
    const size_t remaining = channel.drain_to(rest, 2);
    return remaining == 2 && rest.size() == 2 && rest[0] == 2 && rest[1] == 3 &&
        channel.empty();
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t172_mpsc_bounded_drain");
    const bool boundaryAndFifo = run_boundary_and_fifo();
    const bool concurrentClose = run_concurrent_close();
    const bool sendWaiterProgress = run_send_waiter_progress();
    const bool passed = boundaryAndFifo && concurrentClose && sendWaiterProgress;

    writer.add_test();
    if (passed) {
        writer.add_passed();
    } else {
        writer.add_failed();
    }
    writer.write_result();

    std::cout << "boundary_and_fifo=" << (boundaryAndFifo ? "PASS" : "FAIL")
              << '\n'
              << "concurrent_close=" << (concurrentClose ? "PASS" : "FAIL")
              << '\n'
              << "send_waiter_progress="
              << (sendWaiterProgress ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
