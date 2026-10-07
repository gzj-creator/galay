/**
 * @file t163_mpsc_bounded_hardening.cc
 * @brief 验证 MPSC bounded 的类型约束、容量边界与 tail 游标耗尽行为。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace galay::mpsc {

struct BoundedChannelTestAccess
{
    static size_t normalize_capacity(size_t capacity) noexcept
    {
        return BoundedChannel<int>::normalize_capacity(capacity);
    }

    static constexpr size_t max_ring_capacity() noexcept
    {
        return BoundedChannel<int>::kMaxRingCapacity;
    }

    template <BoundedValue T>
    static void seed_empty_position(BoundedChannel<T>& channel, size_t position)
    {
        channel.m_head.store(position, std::memory_order_relaxed);
        channel.m_tail.store(position, std::memory_order_relaxed);
        const size_t base = position & ~channel.m_mask;
        for (size_t index = 0; index < channel.m_capacity; ++index) {
            size_t sequence = base + index;
            if (sequence < position) {
                sequence += channel.m_capacity;
            }
            channel.m_slots[index].sequence.store(sequence,
                                                  std::memory_order_relaxed);
        }
    }

    template <BoundedValue T>
    static size_t tail_position(const BoundedChannel<T>& channel) noexcept
    {
        return channel.m_tail.load(std::memory_order_acquire) &
            BoundedChannel<T>::kTailPositionMask;
    }

    template <BoundedValue T>
    static void* raw_storage_address(BoundedChannel<T>& channel, size_t index) noexcept
    {
        return channel.m_slots[index].raw_storage();
    }

    template <BoundedValue T>
    static T* live_value_address(BoundedChannel<T>& channel, size_t index) noexcept
    {
        return channel.m_slots[index].value();
    }

    template <BoundedValue T>
    static bool enqueue_recv_waiter(
        BoundedChannel<T>& channel,
        const std::shared_ptr<bounded_detail::ChannelWaiter<T>>& waiter) noexcept
    {
        return channel.enqueue_waiter(channel.m_recvWaiters,
                                     channel.m_recvWaiterCount,
                                     waiter);
    }

    template <BoundedValue T>
    static bool enqueue_send_waiter(
        BoundedChannel<T>& channel,
        const std::shared_ptr<bounded_detail::ChannelWaiter<T>>& waiter) noexcept
    {
        return channel.enqueue_waiter(channel.m_sendWaiters,
                                     channel.m_sendWaiterCount,
                                     waiter);
    }

    template <BoundedValue T>
    static size_t recv_waiter_count(const BoundedChannel<T>& channel) noexcept
    {
        return channel.m_recvWaiterCount.load(std::memory_order_seq_cst);
    }

    template <BoundedValue T>
    static size_t send_waiter_count(const BoundedChannel<T>& channel) noexcept
    {
        return channel.m_sendWaiterCount.load(std::memory_order_seq_cst);
    }
};

} // namespace galay::mpsc

namespace {

struct ThrowingMove
{
    ThrowingMove() noexcept = default;
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&&) noexcept(false) {}
    ThrowingMove& operator=(ThrowingMove&&) noexcept(false)
    {
        return *this;
    }
};

struct NothrowMove
{
    NothrowMove() noexcept = default;
    NothrowMove(const NothrowMove&) = delete;
    NothrowMove& operator=(const NothrowMove&) = delete;
    NothrowMove(NothrowMove&&) noexcept = default;
    NothrowMove& operator=(NothrowMove&&) noexcept = default;
};

static_assert(!galay::mpsc::BoundedValue<ThrowingMove>);
static_assert(galay::mpsc::BoundedValue<NothrowMove>);

struct TrackedValue
{
    int value{0};

    explicit TrackedValue(int input = 0) noexcept : value(input) {}
    TrackedValue(const TrackedValue&) = delete;
    TrackedValue& operator=(const TrackedValue&) = delete;
    TrackedValue(TrackedValue&& other) noexcept
        : value(std::exchange(other.value, -1))
    {
    }
    TrackedValue& operator=(TrackedValue&& other) noexcept
    {
        value = std::exchange(other.value, -1);
        return *this;
    }
};

bool run_capacity_normalization()
{
    const size_t maxCapacity =
        galay::mpsc::BoundedChannelTestAccess::max_ring_capacity();
    return galay::mpsc::BoundedChannelTestAccess::normalize_capacity(0) == 2 &&
        galay::mpsc::BoundedChannelTestAccess::normalize_capacity(3) == 4 &&
        galay::mpsc::BoundedChannelTestAccess::normalize_capacity(
            maxCapacity - 1) == maxCapacity &&
        galay::mpsc::BoundedChannelTestAccess::normalize_capacity(maxCapacity) ==
            maxCapacity &&
        galay::mpsc::BoundedChannelTestAccess::normalize_capacity(
            maxCapacity + 1) == maxCapacity;
}

bool run_storage_lifetime_access()
{
    galay::mpsc::BoundedChannel<TrackedValue> channel(2);
    TrackedValue input(17);
    if (!channel.try_send(std::move(input))) {
        return false;
    }
    void* const raw =
        galay::mpsc::BoundedChannelTestAccess::raw_storage_address(channel, 0);
    TrackedValue* const live =
        galay::mpsc::BoundedChannelTestAccess::live_value_address(channel, 0);
    auto received = channel.try_recv();
    return raw == static_cast<void*>(live) && received.has_value() &&
        received->value == 17;
}

bool run_per_producer_rings()
{
    using Channel = galay::mpsc::BoundedChannel<TrackedValue>;
    Channel zeroProducers(8, 0);
    Channel tooManyProducers(2, 3);
    if (!zeroProducers.is_closed() || !tooManyProducers.is_closed() ||
        zeroProducers.make_producer_token().valid() ||
        tooManyProducers.make_producer_token().valid()) {
        return false;
    }

    Channel channel(8, 2);
    auto first = channel.make_producer_token();
    auto second = channel.make_producer_token();
    auto extra = channel.make_producer_token();
    if (!first.valid() || !second.valid() || extra.valid() ||
        channel.capacity() != 8) {
        return false;
    }

    for (int value = 0; value < 4; ++value) {
        TrackedValue first_value(value);
        TrackedValue secondValue(100 + value);
        if (!channel.try_send(first, std::move(first_value)) ||
            !channel.try_send(second, std::move(secondValue))) {
            return false;
        }
    }
    TrackedValue fullValue(9);
    TrackedValue directValue(10);
    if (channel.try_send(first, std::move(fullValue)) || fullValue.value != 9 ||
        channel.try_send(std::move(directValue)) || directValue.value != 10 ||
        channel.size() != 8 || !channel.full()) {
        return false;
    }
    TrackedValue asyncValue(12);
    auto unsupported = channel.send(std::move(asyncValue));
    if (!unsupported.await_ready()) {
        return false;
    }
    auto unsupportedResult = unsupported.await_resume();
    if (unsupportedResult.has_value() ||
        !galay::kernel::IOError::contains(unsupportedResult.error().code(),
                                         galay::kernel::kNotReady)) {
        return false;
    }

    channel.close();
    TrackedValue closedValue(11);
    if (channel.try_send(second, std::move(closedValue)) ||
        closedValue.value != 11) {
        return false;
    }

    std::array<int, 2> expected{0, 100};
    for (size_t received = 0; received < 8; ++received) {
        auto value = channel.try_recv();
        if (!value.has_value()) {
            return false;
        }
        const size_t producer = value->value >= 100 ? 1 : 0;
        if (value->value != expected[producer]++) {
            return false;
        }
    }
    return channel.empty() && !channel.try_recv().has_value();
}

bool run_concurrent_per_producer_rings()
{
    using Channel = galay::mpsc::BoundedChannel<uint64_t>;
    constexpr size_t kProducerCount = 2;
    constexpr uint64_t kMessagesPerProducer = 20'000;
    Channel channel(1024, kProducerCount);
    std::array<Channel::ProducerToken, kProducerCount> tokens{
        channel.make_producer_token(), channel.make_producer_token()};
    if (!tokens[0].valid() || !tokens[1].valid()) {
        return false;
    }

    std::atomic<bool> start{false};
    std::array<std::thread, kProducerCount> producers;
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers[producer] = std::thread([&, producer]() {
            start.wait(false, std::memory_order_acquire);
            for (uint64_t sequence = 0;
                 sequence < kMessagesPerProducer;
                 ++sequence) {
                uint64_t value =
                    (static_cast<uint64_t>(producer) << 32U) | sequence;
                while (!channel.try_send(tokens[producer], std::move(value))) {
                    std::this_thread::yield();
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    start.notify_all();

    std::array<uint64_t, kProducerCount> expected{};
    uint64_t received = 0;
    bool fifoOk = true;
    while (received < kProducerCount * kMessagesPerProducer) {
        auto value = channel.try_recv();
        if (!value.has_value()) {
            std::this_thread::yield();
            continue;
        }
        const size_t producer = static_cast<size_t>(*value >> 32U);
        const uint64_t sequence = *value & 0xffff'ffffULL;
        if (producer >= kProducerCount) {
            fifoOk = false;
        } else if (sequence != expected[producer]++) {
            fifoOk = false;
        }
        ++received;
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    channel.close();
    return fifoOk && channel.empty();
}

bool run_active_waiter_accounting()
{
    galay::mpsc::BoundedChannel<int> recvChannel(2);
    auto recvWaiter =
        std::make_shared<galay::mpsc::bounded_detail::ChannelWaiter<int>>(
            galay::kernel::Waker());
    if (!galay::mpsc::BoundedChannelTestAccess::enqueue_recv_waiter(
            recvChannel, recvWaiter) ||
        galay::mpsc::BoundedChannelTestAccess::recv_waiter_count(recvChannel) !=
            1 ||
        !recvChannel.try_send(21) ||
        recvWaiter->state.load(std::memory_order_acquire) !=
            galay::mpsc::bounded_detail::WaiterState::kFulfilled ||
        !recvWaiter->value.has_value() || *recvWaiter->value != 21 ||
        galay::mpsc::BoundedChannelTestAccess::recv_waiter_count(recvChannel) !=
            0 ||
        !recvChannel.empty()) {
        return false;
    }

    galay::mpsc::BoundedChannel<int> sendChannel(2);
    if (!sendChannel.try_send(31) || !sendChannel.try_send(32)) {
        return false;
    }
    auto sendWaiter =
        std::make_shared<galay::mpsc::bounded_detail::ChannelWaiter<int>>(
            galay::kernel::Waker());
    sendWaiter->value.emplace(33);
    if (!galay::mpsc::BoundedChannelTestAccess::enqueue_send_waiter(
            sendChannel, sendWaiter) ||
        galay::mpsc::BoundedChannelTestAccess::send_waiter_count(sendChannel) !=
            1) {
        return false;
    }
    auto first = sendChannel.try_recv();
    auto rest = sendChannel.try_recv_batch(2);
    return first.has_value() && *first == 31 && rest.has_value() &&
        rest->size() == 2 && (*rest)[0] == 32 && (*rest)[1] == 33 &&
        sendWaiter->state.load(std::memory_order_acquire) ==
            galay::mpsc::bounded_detail::WaiterState::kFulfilled &&
        !sendWaiter->value.has_value() &&
        galay::mpsc::BoundedChannelTestAccess::send_waiter_count(sendChannel) ==
            0;
}

bool run_tail_close_bit_boundary()
{
    constexpr size_t kPositionMask =
        (size_t{1} << (sizeof(size_t) * 8U - 1U)) - 1U;

    galay::mpsc::BoundedChannel<TrackedValue> lastReservation(2);
    galay::mpsc::BoundedChannelTestAccess::seed_empty_position(
        lastReservation, kPositionMask - 1U);
    TrackedValue finalValue(91);
    if (!lastReservation.try_send(std::move(finalValue)) ||
        lastReservation.is_closed() || finalValue.value != -1 ||
        galay::mpsc::BoundedChannelTestAccess::tail_position(lastReservation) !=
            kPositionMask) {
        return false;
    }
    lastReservation.close();
    auto received = lastReservation.try_recv();
    if (!lastReservation.is_closed() || !received.has_value() ||
        received->value != 91 || !lastReservation.empty()) {
        return false;
    }

    galay::mpsc::BoundedChannel<TrackedValue> exhausted(2);
    galay::mpsc::BoundedChannelTestAccess::seed_empty_position(exhausted,
                                                            kPositionMask);
    TrackedValue rejected(92);
    return !exhausted.try_send(std::move(rejected)) && rejected.value == 92 &&
        exhausted.is_closed() && exhausted.try_recv() == std::nullopt &&
        galay::mpsc::BoundedChannelTestAccess::tail_position(exhausted) ==
            kPositionMask;
}

} // namespace

int main()
{
    if (!run_capacity_normalization()) {
        std::cerr << "[T163] MPSC bounded capacity normalization failed\n";
        return 1;
    }
    if (!run_storage_lifetime_access()) {
        std::cerr << "[T163] MPSC bounded slot lifetime access failed\n";
        return 1;
    }
    if (!run_per_producer_rings()) {
        std::cerr << "[T163] MPSC bounded per-producer ring failed\n";
        return 1;
    }
    if (!run_concurrent_per_producer_rings()) {
        std::cerr << "[T163] MPSC bounded concurrent per-producer ring failed\n";
        return 1;
    }
    if (!run_active_waiter_accounting()) {
        std::cerr << "[T163] MPSC bounded active waiter accounting failed\n";
        return 1;
    }
    if (!run_tail_close_bit_boundary()) {
        std::cerr << "[T163] MPSC bounded tail exhaustion hardening failed\n";
        return 1;
    }
    return 0;
}
