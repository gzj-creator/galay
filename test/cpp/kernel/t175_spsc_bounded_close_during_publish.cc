/**
 * @file t175_spsc_bounded_close_during_publish.cc
 * @brief 验证 close 与 SPSC slot 构造发布并发时保留尾消息。
 */

#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

struct PublishGate {
    std::atomic<bool> shouldBlock{true};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

struct BlockingMoveValue {
    PublishGate* gate = nullptr;
    int value = 0;

    BlockingMoveValue(PublishGate* inputGate, int input) noexcept
        : gate(inputGate), value(input) {}
    BlockingMoveValue(const BlockingMoveValue&) = delete;
    BlockingMoveValue& operator=(const BlockingMoveValue&) = delete;

    BlockingMoveValue(BlockingMoveValue&& other) noexcept
        : gate(other.gate), value(std::exchange(other.value, -1))
    {
        if (gate != nullptr &&
            gate->shouldBlock.exchange(false, std::memory_order_acq_rel)) {
            gate->entered.store(true, std::memory_order_release);
            bool released = gate->release.load(std::memory_order_acquire);
            while (!released) {
                gate->release.wait(false, std::memory_order_acquire);
                released = gate->release.load(std::memory_order_acquire);
            }
        }
    }

    BlockingMoveValue& operator=(BlockingMoveValue&& other) noexcept
    {
        gate = other.gate;
        value = std::exchange(other.value, -1);
        return *this;
    }
};

} // namespace

int main()
{
    PublishGate gate;
    galay::spsc::BoundedChannel<BlockingMoveValue> channel(2);
    BlockingMoveValue input(&gate, 73);
    std::atomic<bool> sent{false};
    std::thread producer([&]() {
        sent.store(channel.trySend(std::move(input)), std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!gate.entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!gate.entered.load(std::memory_order_acquire)) {
        gate.release.store(true, std::memory_order_release);
        gate.release.notify_one();
        producer.join();
        std::cerr << "T175 producer did not enter slot construction\n";
        return 1;
    }

    channel.close();
    gate.release.store(true, std::memory_order_release);
    gate.release.notify_one();
    producer.join();

    auto value = channel.tryRecv();
    const bool passed = sent.load(std::memory_order_acquire) &&
        channel.isClosed() && value.has_value() && value->value == 73 &&
        !channel.tryRecv().has_value();
    if (!passed) {
        std::cerr << "T175 close-during-publish lost the tail message\n";
        return 1;
    }
    std::cout << "T175-SpscBoundedCloseDuringPublish PASS\n";
    return 0;
}
