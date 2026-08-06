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
    static size_t normalizeCapacity(size_t capacity) noexcept
    {
        return BoundedChannel<int>::normalizeCapacity(capacity);
    }

    static constexpr size_t maxRingCapacity() noexcept
    {
        return BoundedChannel<int>::kMaxRingCapacity;
    }

    template <BoundedValue T>
    static void seedEmptyPosition(BoundedChannel<T>& channel, size_t position)
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
    static size_t tailPosition(const BoundedChannel<T>& channel) noexcept
    {
        return channel.m_tail.load(std::memory_order_acquire) &
            BoundedChannel<T>::kTailPositionMask;
    }

    template <BoundedValue T>
    static void* rawStorageAddress(BoundedChannel<T>& channel, size_t index) noexcept
    {
        return channel.m_slots[index].rawStorage();
    }

    template <BoundedValue T>
    static T* liveValueAddress(BoundedChannel<T>& channel, size_t index) noexcept
    {
        return channel.m_slots[index].value();
    }

    template <BoundedValue T>
    static bool enqueueRecvWaiter(
        BoundedChannel<T>& channel,
        const std::shared_ptr<bounded_detail::ChannelWaiter<T>>& waiter) noexcept
    {
        return channel.enqueueWaiter(channel.m_recvWaiters,
                                     channel.m_recvWaiterCount,
                                     waiter);
    }

    template <BoundedValue T>
    static bool enqueueSendWaiter(
        BoundedChannel<T>& channel,
        const std::shared_ptr<bounded_detail::ChannelWaiter<T>>& waiter) noexcept
    {
        return channel.enqueueWaiter(channel.m_sendWaiters,
                                     channel.m_sendWaiterCount,
                                     waiter);
    }

    template <BoundedValue T>
    static size_t recvWaiterCount(const BoundedChannel<T>& channel) noexcept
    {
        return channel.m_recvWaiterCount.load(std::memory_order_seq_cst);
    }

    template <BoundedValue T>
    static size_t sendWaiterCount(const BoundedChannel<T>& channel) noexcept
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

bool runCapacityNormalization()
{
    const size_t maxCapacity =
        galay::mpsc::BoundedChannelTestAccess::maxRingCapacity();
    return galay::mpsc::BoundedChannelTestAccess::normalizeCapacity(0) == 2 &&
        galay::mpsc::BoundedChannelTestAccess::normalizeCapacity(3) == 4 &&
        galay::mpsc::BoundedChannelTestAccess::normalizeCapacity(
            maxCapacity - 1) == maxCapacity &&
        galay::mpsc::BoundedChannelTestAccess::normalizeCapacity(maxCapacity) ==
            maxCapacity &&
        galay::mpsc::BoundedChannelTestAccess::normalizeCapacity(
            maxCapacity + 1) == maxCapacity;
}

bool runStorageLifetimeAccess()
{
    galay::mpsc::BoundedChannel<TrackedValue> channel(2);
    TrackedValue input(17);
    if (!channel.trySend(std::move(input))) {
        return false;
    }
    void* const raw =
        galay::mpsc::BoundedChannelTestAccess::rawStorageAddress(channel, 0);
    TrackedValue* const live =
        galay::mpsc::BoundedChannelTestAccess::liveValueAddress(channel, 0);
    auto received = channel.tryRecv();
    return raw == static_cast<void*>(live) && received.has_value() &&
        received->value == 17;
}

bool runPerProducerRings()
{
    using Channel = galay::mpsc::BoundedChannel<TrackedValue>;
    Channel zeroProducers(8, 0);
    Channel tooManyProducers(2, 3);
    if (!zeroProducers.isClosed() || !tooManyProducers.isClosed() ||
        zeroProducers.makeProducerToken().valid() ||
        tooManyProducers.makeProducerToken().valid()) {
        return false;
    }

    Channel channel(8, 2);
    auto first = channel.makeProducerToken();
    auto second = channel.makeProducerToken();
    auto extra = channel.makeProducerToken();
    if (!first.valid() || !second.valid() || extra.valid() ||
        channel.capacity() != 8) {
        return false;
    }

    for (int value = 0; value < 4; ++value) {
        TrackedValue firstValue(value);
        TrackedValue secondValue(100 + value);
        if (!channel.trySend(first, std::move(firstValue)) ||
            !channel.trySend(second, std::move(secondValue))) {
            return false;
        }
    }
    TrackedValue fullValue(9);
    TrackedValue directValue(10);
    if (channel.trySend(first, std::move(fullValue)) || fullValue.value != 9 ||
        channel.trySend(std::move(directValue)) || directValue.value != 10 ||
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
    if (channel.trySend(second, std::move(closedValue)) ||
        closedValue.value != 11) {
        return false;
    }

    std::array<int, 2> expected{0, 100};
    for (size_t received = 0; received < 8; ++received) {
        auto value = channel.tryRecv();
        if (!value.has_value()) {
            return false;
        }
        const size_t producer = value->value >= 100 ? 1 : 0;
        if (value->value != expected[producer]++) {
            return false;
        }
    }
    return channel.empty() && !channel.tryRecv().has_value();
}

bool runConcurrentPerProducerRings()
{
    using Channel = galay::mpsc::BoundedChannel<uint64_t>;
    constexpr size_t kProducerCount = 2;
    constexpr uint64_t kMessagesPerProducer = 20'000;
    Channel channel(1024, kProducerCount);
    std::array<Channel::ProducerToken, kProducerCount> tokens{
        channel.makeProducerToken(), channel.makeProducerToken()};
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
                while (!channel.trySend(tokens[producer], std::move(value))) {
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
        auto value = channel.tryRecv();
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

bool runActiveWaiterAccounting()
{
    galay::mpsc::BoundedChannel<int> recvChannel(2);
    auto recvWaiter =
        std::make_shared<galay::mpsc::bounded_detail::ChannelWaiter<int>>(
            galay::kernel::Waker());
    if (!galay::mpsc::BoundedChannelTestAccess::enqueueRecvWaiter(
            recvChannel, recvWaiter) ||
        galay::mpsc::BoundedChannelTestAccess::recvWaiterCount(recvChannel) !=
            1 ||
        !recvChannel.trySend(21) ||
        recvWaiter->state.load(std::memory_order_acquire) !=
            galay::mpsc::bounded_detail::WaiterState::kFulfilled ||
        !recvWaiter->value.has_value() || *recvWaiter->value != 21 ||
        galay::mpsc::BoundedChannelTestAccess::recvWaiterCount(recvChannel) !=
            0 ||
        !recvChannel.empty()) {
        return false;
    }

    galay::mpsc::BoundedChannel<int> sendChannel(2);
    if (!sendChannel.trySend(31) || !sendChannel.trySend(32)) {
        return false;
    }
    auto sendWaiter =
        std::make_shared<galay::mpsc::bounded_detail::ChannelWaiter<int>>(
            galay::kernel::Waker());
    sendWaiter->value.emplace(33);
    if (!galay::mpsc::BoundedChannelTestAccess::enqueueSendWaiter(
            sendChannel, sendWaiter) ||
        galay::mpsc::BoundedChannelTestAccess::sendWaiterCount(sendChannel) !=
            1) {
        return false;
    }
    auto first = sendChannel.tryRecv();
    auto rest = sendChannel.tryRecvBatch(2);
    return first.has_value() && *first == 31 && rest.has_value() &&
        rest->size() == 2 && (*rest)[0] == 32 && (*rest)[1] == 33 &&
        sendWaiter->state.load(std::memory_order_acquire) ==
            galay::mpsc::bounded_detail::WaiterState::kFulfilled &&
        !sendWaiter->value.has_value() &&
        galay::mpsc::BoundedChannelTestAccess::sendWaiterCount(sendChannel) ==
            0;
}

bool runTailCloseBitBoundary()
{
    constexpr size_t kPositionMask =
        (size_t{1} << (sizeof(size_t) * 8U - 1U)) - 1U;

    galay::mpsc::BoundedChannel<TrackedValue> lastReservation(2);
    galay::mpsc::BoundedChannelTestAccess::seedEmptyPosition(
        lastReservation, kPositionMask - 1U);
    TrackedValue finalValue(91);
    if (!lastReservation.trySend(std::move(finalValue)) ||
        lastReservation.isClosed() || finalValue.value != -1 ||
        galay::mpsc::BoundedChannelTestAccess::tailPosition(lastReservation) !=
            kPositionMask) {
        return false;
    }
    lastReservation.close();
    auto received = lastReservation.tryRecv();
    if (!lastReservation.isClosed() || !received.has_value() ||
        received->value != 91 || !lastReservation.empty()) {
        return false;
    }

    galay::mpsc::BoundedChannel<TrackedValue> exhausted(2);
    galay::mpsc::BoundedChannelTestAccess::seedEmptyPosition(exhausted,
                                                            kPositionMask);
    TrackedValue rejected(92);
    return !exhausted.trySend(std::move(rejected)) && rejected.value == 92 &&
        exhausted.isClosed() && exhausted.tryRecv() == std::nullopt &&
        galay::mpsc::BoundedChannelTestAccess::tailPosition(exhausted) ==
            kPositionMask;
}

} // namespace

int main()
{
    if (!runCapacityNormalization()) {
        std::cerr << "[T163] MPSC bounded capacity normalization failed\n";
        return 1;
    }
    if (!runStorageLifetimeAccess()) {
        std::cerr << "[T163] MPSC bounded slot lifetime access failed\n";
        return 1;
    }
    if (!runPerProducerRings()) {
        std::cerr << "[T163] MPSC bounded per-producer ring failed\n";
        return 1;
    }
    if (!runConcurrentPerProducerRings()) {
        std::cerr << "[T163] MPSC bounded concurrent per-producer ring failed\n";
        return 1;
    }
    if (!runActiveWaiterAccounting()) {
        std::cerr << "[T163] MPSC bounded active waiter accounting failed\n";
        return 1;
    }
    if (!runTailCloseBitBoundary()) {
        std::cerr << "[T163] MPSC bounded tail exhaustion hardening failed\n";
        return 1;
    }
    return 0;
}
