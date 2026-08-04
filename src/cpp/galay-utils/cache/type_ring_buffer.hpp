/**
 * @file type_ring_buffer.hpp
 * @brief typed 单生产者单消费者环形缓冲区
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 生产者和消费者各自独占本地单调游标，并缓存对端发布游标。只有在
 * 看似已满或已空时才 acquire 刷新缓存；槽位所有权通过 release/acquire 发布，
 * 稳态成功路径不使用逐槽原子、CAS、原子 RMW 或锁。
 */

#ifndef GALAY_UTILS_CACHE_TYPE_RING_BUFFER_HPP
#define GALAY_UTILS_CACHE_TYPE_RING_BUFFER_HPP

#include "../common/defn.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace galay::utils
{

/**
 * @brief 约束 TypeRingBuffer 可存储的元素类型。
 * @details 发送、接收和销毁均位于无异常数据面，因此移动构造与析构必须保证
 * 不抛出异常；写入调用方对象的接收接口另行约束不抛移动赋值。
 */
template <typename T>
concept TypeRingBufferValue = std::is_object_v<T> &&
    std::same_as<T, std::remove_cv_t<T>> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

/** @brief 约束 TypeRingBuffer 使用无锁无符号整数游标。 */
template <typename Cursor>
concept TypeRingBufferCursor = std::unsigned_integral<Cursor> &&
    (!std::same_as<std::remove_cv_t<Cursor>, bool>) &&
    std::atomic<Cursor>::is_always_lock_free;

namespace detail {

template <TypeRingBufferCursor Cursor>
inline constexpr size_t kTypeRingBufferMaximumCapacity = size_t{1} <<
    (std::min(std::numeric_limits<Cursor>::digits,
              std::numeric_limits<size_t>::digits) - 1U);

template <size_t Capacity, typename Cursor>
concept ValidTypeRingBufferCapacity = TypeRingBufferCursor<Cursor> &&
    (Capacity == std::dynamic_extent ||
     (Capacity >= 2 && std::has_single_bit(Capacity) &&
      Capacity <= kTypeRingBufferMaximumCapacity<Cursor>));

} // namespace detail

/** @brief TypeRingBuffer 构造结果。 */
enum class TypeRingBufferError : uint8_t {
    kNone,             ///< 构造成功，可以进入数据面。
    kCapacityTooLarge, ///< 规范化容量超出游标可无歧义表达的上限。
    kAllocationFailed, ///< 槽位存储分配失败。
};

/**
 * @brief 获取 TypeRingBufferError 的静态错误字符串。
 * @param error 错误枚举。
 * @return 覆盖所有公开枚举值的非空字符串。
 */
[[nodiscard]] inline const char*
typeRingBufferErrorString(TypeRingBufferError error) noexcept
{
    switch (error) {
    case TypeRingBufferError::kNone:
        return "none";
    case TypeRingBufferError::kCapacityTooLarge:
        return "capacity too large";
    case TypeRingBufferError::kAllocationFailed:
        return "allocation failed";
    }
    return "unknown type ring buffer error";
}

/**
 * @brief 支持运行时或编译期容量的 typed SPSC ring 数据面。
 * @tparam T 元素类型，必须满足 TypeRingBufferValue。
 * @tparam Capacity std::dynamic_extent 表示运行时容量；否则表示由对象成员持有的
 *         编译期容量，必须是不小于 2 的安全 2 次幂。
 * @tparam Cursor 单调无符号游标类型；默认 size_t，窄类型可用于回绕测试。
 *
 * @details
 * - 只能有一个逻辑生产者调用 trySend() 或 trySendBatch()。
 * - 只能有一个逻辑消费者调用接收接口，多个接收接口不得并发。
 * - producer 构造槽位后 release 发布 tail，consumer acquire 后读取。
 * - consumer 搬出并销毁槽位后 release 发布 head，producer acquire 后复用。
 * - 两侧本地游标和对端缓存均由所属一侧独占，不需要原子 RMW。
 *
 * @note 对象具有唯一并发身份，不可复制或移动；析构前必须停止两侧访问。
 * @note 构造不抛异常；使用数据面 API 前必须确认 error() == kNone。
 */
template <TypeRingBufferValue T,
          size_t Capacity = std::dynamic_extent,
          TypeRingBufferCursor Cursor = size_t>
    requires detail::ValidTypeRingBufferCapacity<Capacity, Cursor>
class TypeRingBuffer
{
private:
    static constexpr bool kUsesStaticCapacity =
        Capacity != std::dynamic_extent;

public:
    /**
     * @brief 创建指定容量的 ring。
     * @param capacity 期望容量；小于等于 2 时取 2，其余向上取整为 2 的幂。
     * @details 分配失败或容量超过 Cursor 的安全范围时保持无异常，具体原因由
     * error() 返回；成功后 capacity() 返回实际容量。
     * @note 仅运行时容量 specialization 提供该构造函数。
     */
    explicit TypeRingBuffer(size_t capacity) noexcept
        requires (!kUsesStaticCapacity)
    {
        const size_t normalized = normalizeCapacity(capacity);
        if (normalized == 0) {
            m_error = TypeRingBufferError::kCapacityTooLarge;
            return;
        }

        Slot* const slots = new (std::nothrow) Slot[normalized];
        if (slots == nullptr) {
            m_error = TypeRingBufferError::kAllocationFailed;
            return;
        }
        m_slots.reset(slots);
        initializeLocalStorage(slots, normalized);
    }

    /**
     * @brief 创建成员内持有槽位的编译期容量 ring。
     * @details 该构造不分配内存，也不清零尚未开始 T 生命周期的槽位；容量由
     *          Capacity 唯一确定。
     */
    TypeRingBuffer() noexcept
        requires kUsesStaticCapacity
    {
        initializeLocalStorage(m_slots.data(), Capacity);
    }

    /**
     * @brief 销毁尚未消费的元素并释放槽位。
     * @pre 不得再有生产者或消费者并发访问本对象。
     */
    ~TypeRingBuffer() noexcept
    {
        if constexpr (!kUsesStaticCapacity) {
            if (m_slots == nullptr) {
                return;
            }
        }
        const Cursor head = m_head.value.load(std::memory_order_relaxed);
        const Cursor tail = m_tail.value.load(std::memory_order_relaxed);
        const LocalCursor& consumer = m_consumerLocal;
        const size_t pending = cursorDistance(tail, head);
        for (size_t offset = 0; offset < pending; ++offset) {
            const size_t index =
                (static_cast<size_t>(head) + offset) & consumer.mask;
            std::destroy_at(consumer.slots[index].value());
        }
    }

    /** @brief 禁止复制构造；ring 具有唯一并发身份。 */
    TypeRingBuffer(const TypeRingBuffer&) = delete;

    /** @brief 禁止复制赋值；ring 具有唯一并发身份。 */
    TypeRingBuffer& operator=(const TypeRingBuffer&) = delete;

    /** @brief 禁止移动构造，避免运行中的调用方持有失效地址。 */
    TypeRingBuffer(TypeRingBuffer&&) = delete;

    /** @brief 禁止移动赋值，避免运行中的调用方持有失效地址。 */
    TypeRingBuffer& operator=(TypeRingBuffer&&) = delete;

    /**
     * @brief 返回构造状态。
     * @return kNone 表示可使用；其余值表示容量无效或内存分配失败。
     * @note 构造完成后状态不再变化，可由任意线程读取。
     * @note 编译期容量 specialization 始终返回 kNone。
     */
    [[nodiscard]] TypeRingBufferError error() const noexcept
    {
        return m_error;
    }

    /**
     * @brief 返回实际容量。
     * @return 构造成功时返回不小于 2 的 2 的幂；失败时返回 0。
     */
    [[nodiscard]] size_t capacity() const noexcept
    {
        return m_producerLocal.capacity;
    }

    /**
     * @brief 尝试发布一条消息。
     * @param value 待发布值；仅在确认存在空闲槽位后移动。
     * @return 成功返回 true；当前 ring 已满时返回 false，value 保持未移动。
     * @pre error() == kNone，且只能由唯一逻辑生产者调用。
     * @note 该函数不阻塞；常规成功路径仅执行一次 tail release store。
     */
    [[nodiscard]] bool trySend(T&& value) noexcept
    {
        return trySendOne(m_producerLocal, m_head, m_tail, std::move(value));
    }

    /**
     * @brief 尽量把输入前缀批量发布到 ring。
     * @param values 待发送的调用方存储；仅成功发布的前缀会被移动。
     * @return 实际发布数量，范围为 [0, values.size()]。
     * @pre error() == kNone，且只能由唯一逻辑生产者调用。
     * @note 整批构造完成后只执行一次 tail release store。
     */
    [[nodiscard]] size_t trySendBatch(std::span<T> values) noexcept
    {
        if (values.empty()) {
            return 0;
        }

        LocalCursor& producer = m_producerLocal;
        const Cursor tail = producer.position;
        size_t used = cursorDistance(tail, producer.cachedPeer);
        if (used >= producer.capacity ||
            values.size() > producer.capacity - used) {
            producer.cachedPeer =
                m_head.value.load(std::memory_order_acquire);
            used = cursorDistance(tail, producer.cachedPeer);
            if (used >= producer.capacity) {
                return 0;
            }
        }

        const size_t count = std::min(values.size(), producer.capacity - used);
        for (size_t offset = 0; offset < count; ++offset) {
            const size_t index =
                (static_cast<size_t>(tail) + offset) & producer.mask;
            [[maybe_unused]] T* const stored = std::construct_at(
                producer.slots[index].storageAddress(),
                std::move(values[offset]));
        }
        const Cursor next = static_cast<Cursor>(tail + static_cast<Cursor>(count));
        producer.position = next;
        m_tail.value.store(next, std::memory_order_release);
        return count;
    }

    /**
     * @brief 尝试接收一条消息。
     * @return 当前有消息时返回其所有权；为空时返回 std::nullopt。
     * @pre error() == kNone，且只能由唯一逻辑消费者调用。
     * @note 该函数不阻塞；常规成功路径仅执行一次 head release store。
     */
    [[nodiscard]] std::optional<T> tryRecv() noexcept
    {
        return tryRecvOne(m_consumerLocal, m_head, m_tail);
    }

    /**
     * @brief 尝试把一条消息移动到调用方拥有的已构造对象。
     * @param output 接收目标，仅成功时被移动赋值。
     * @return 成功接收返回 true；当前为空返回 false。
     * @pre error() == kNone，且只能由唯一逻辑消费者调用。
     */
    [[nodiscard]] bool tryRecv(T& output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        return tryRecvOne(m_consumerLocal, m_head, m_tail, output);
    }

    /**
     * @brief 将当前可用消息批量搬入调用方存储。
     * @param output 已构造的目标元素区间，由调用方拥有并保证调用期间有效。
     * @return 实际搬运数量，范围为 [0, output.size()]。
     * @pre error() == kNone，且只能由唯一逻辑消费者调用。
     * @note 批次保持 FIFO，并在整批搬运和销毁后只执行一次 head release store。
     */
    [[nodiscard]] size_t tryRecvBatch(std::span<T> output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        if (output.empty()) {
            return 0;
        }

        LocalCursor& consumer = m_consumerLocal;
        const Cursor head = consumer.position;
        const Cursor tail = m_tail.value.load(std::memory_order_acquire);
        consumer.cachedPeer = tail;
        const size_t count = std::min(output.size(), cursorDistance(tail, head));
        if (count == 0) {
            return 0;
        }
        for (size_t offset = 0; offset < count; ++offset) {
            const size_t index =
                (static_cast<size_t>(head) + offset) & consumer.mask;
            Slot& slot = consumer.slots[index];
            output[offset] = std::move(*slot.value());
            std::destroy_at(slot.value());
        }

        const Cursor next = static_cast<Cursor>(head + static_cast<Cursor>(count));
        consumer.position = next;
        m_head.value.store(next, std::memory_order_release);
        return count;
    }

private:
#if defined(GALAY_ARCH_X64)
    static constexpr size_t kPublishedCursorAlignment = 2 * kCacheLineSize;
#else
    static constexpr size_t kPublishedCursorAlignment = kCacheLineSize;
#endif

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];

        T* storageAddress() noexcept
        {
            return reinterpret_cast<T*>(storage);
        }

        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    static constexpr size_t kMaximumArrayCapacity = std::bit_floor(
        std::numeric_limits<size_t>::max() / sizeof(Slot));
    static constexpr size_t kMaximumCapacity = std::min(
        detail::kTypeRingBufferMaximumCapacity<Cursor>, kMaximumArrayCapacity);

    /** @brief 共享发布游标隔离空间预取的相邻缓存行。 */
    struct alignas(kPublishedCursorAlignment) PublishedCursor
    {
        std::atomic<Cursor> value{0};
    };

    /** @brief 单侧独占的本地游标和对端发布游标缓存。 */
    template <size_t Alignment>
    struct alignas(Alignment) CursorState
    {
        Slot* slots = nullptr;
        size_t capacity = 0;
        size_t mask = 0;
        Cursor position = 0;
        Cursor cachedPeer = 0;
    };

    using LocalCursor = CursorState<kCacheLineSize>;

    /**
     * @brief 独占端点持有的线程本地游标，不与另一端共享缓存线。
     * @details Endpoint 常被 std::thread 的 heap closure 持有；x64 使用两条
     *          缓存行的对齐距离，其他架构使用公共缓存行大小。
     */
    using EndpointCursor = CursorState<kPublishedCursorAlignment>;

    template <typename State>
    [[nodiscard]] static bool trySendOne(State& producer,
                                         PublishedCursor& head,
                                         PublishedCursor& tail,
                                         T&& value) noexcept
    {
        const Cursor position = producer.position;
        if (cursorDistance(position, producer.cachedPeer) >= producer.capacity) {
            producer.cachedPeer = head.value.load(std::memory_order_acquire);
            if (cursorDistance(position, producer.cachedPeer) >= producer.capacity) {
                return false;
            }
        }

        Slot& slot =
            producer.slots[static_cast<size_t>(position) & producer.mask];
        [[maybe_unused]] T* const stored =
            std::construct_at(slot.storageAddress(), std::move(value));
        const Cursor next = static_cast<Cursor>(position + 1);
        producer.position = next;
        tail.value.store(next, std::memory_order_release);
        return true;
    }

    template <typename State>
    [[nodiscard]] static std::optional<T> tryRecvOne(
        State& consumer,
        PublishedCursor& head,
        PublishedCursor& tail) noexcept
    {
        const Cursor position = consumer.position;
        if (position == consumer.cachedPeer) {
            consumer.cachedPeer = tail.value.load(std::memory_order_acquire);
            if (position == consumer.cachedPeer) {
                return std::nullopt;
            }
        }

        Slot& slot =
            consumer.slots[static_cast<size_t>(position) & consumer.mask];
        std::optional<T> value(std::in_place, std::move(*slot.value()));
        std::destroy_at(slot.value());
        const Cursor next = static_cast<Cursor>(position + 1);
        consumer.position = next;
        head.value.store(next, std::memory_order_release);
        return value;
    }

    template <typename State>
    [[nodiscard]] static bool tryRecvOne(State& consumer,
                                         PublishedCursor& head,
                                         PublishedCursor& tail,
                                         T& output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        const Cursor position = consumer.position;
        if (position == consumer.cachedPeer) {
            consumer.cachedPeer = tail.value.load(std::memory_order_acquire);
            if (position == consumer.cachedPeer) {
                return false;
            }
        }

        Slot& slot =
            consumer.slots[static_cast<size_t>(position) & consumer.mask];
        output = std::move(*slot.value());
        std::destroy_at(slot.value());
        const Cursor next = static_cast<Cursor>(position + 1);
        consumer.position = next;
        head.value.store(next, std::memory_order_release);
        return true;
    }

public:
    /**
     * @brief 借用 ring 的唯一 producer 端点。
     * @note 端点不可复制；存活期间 ring 必须保持有效，且不得再调用 ring 的发送接口。
     * @note 移动后源端点失效，不得继续调用。
     */
    class Producer
    {
    public:
        Producer(const Producer&) = delete;
        Producer& operator=(const Producer&) = delete;
        Producer(Producer&& other) noexcept
            : m_ring(std::exchange(other.m_ring, nullptr)),
              m_local(other.m_local)
        {
        }

        Producer& operator=(Producer&& other) noexcept
        {
            if (this != &other) {
                m_ring = std::exchange(other.m_ring, nullptr);
                m_local = other.m_local;
            }
            return *this;
        }

        /** @brief 尝试发布一条消息；满时返回 false 且 value 保持未移动。 */
        [[nodiscard]] bool trySend(T&& value) noexcept
        {
            return trySendOne(
                m_local, m_ring->m_head, m_ring->m_tail, std::move(value));
        }

    private:
        friend class TypeRingBuffer;

        explicit Producer(TypeRingBuffer* ring) noexcept
            : m_ring(ring),
              m_local{ring->m_producerLocal.slots,
                      ring->m_producerLocal.capacity,
                      ring->m_producerLocal.mask,
                      ring->m_tail.value.load(std::memory_order_relaxed),
                      ring->m_head.value.load(std::memory_order_relaxed)}
        {
        }

        TypeRingBuffer* m_ring = nullptr;
        EndpointCursor m_local;
    };

    /**
     * @brief 借用 ring 的唯一 consumer 端点。
     * @note 端点不可复制；存活期间 ring 必须保持有效，且不得再调用 ring 的接收接口。
     * @note 移动后源端点失效，不得继续调用。
     */
    class Consumer
    {
    public:
        Consumer(const Consumer&) = delete;
        Consumer& operator=(const Consumer&) = delete;
        Consumer(Consumer&& other) noexcept
            : m_ring(std::exchange(other.m_ring, nullptr)),
              m_local(other.m_local)
        {
        }

        Consumer& operator=(Consumer&& other) noexcept
        {
            if (this != &other) {
                m_ring = std::exchange(other.m_ring, nullptr);
                m_local = other.m_local;
            }
            return *this;
        }

        /** @brief 尝试接收一条消息；空时返回 std::nullopt。 */
        [[nodiscard]] std::optional<T> tryRecv() noexcept
        {
            return tryRecvOne(m_local, m_ring->m_head, m_ring->m_tail);
        }

        /** @brief 尝试把一条消息移动到调用方对象；空时返回 false。 */
        [[nodiscard]] bool tryRecv(T& output) noexcept
            requires std::is_nothrow_move_assignable_v<T>
        {
            return tryRecvOne(
                m_local, m_ring->m_head, m_ring->m_tail, output);
        }

    private:
        friend class TypeRingBuffer;

        explicit Consumer(TypeRingBuffer* ring) noexcept
            : m_ring(ring),
              m_local{ring->m_consumerLocal.slots,
                      ring->m_consumerLocal.capacity,
                      ring->m_consumerLocal.mask,
                      ring->m_head.value.load(std::memory_order_relaxed),
                      ring->m_tail.value.load(std::memory_order_relaxed)}
        {
        }

        TypeRingBuffer* m_ring = nullptr;
        EndpointCursor m_local;
    };

    /** @brief 一次拆分得到的唯一 producer/consumer 端点。 */
    struct Endpoints
    {
        Producer producer;
        Consumer consumer;
    };

    /**
     * @brief 为两个线程创建寄存器友好的独占端点。
     * @return 借用当前 ring 的唯一 producer 和 consumer。
     * @pre error() == kNone，调用期间没有并发访问，且之后不与 ring 直接 API 混用。
     * @note ring 必须比返回的两个端点活得更久。
     */
    [[nodiscard]] Endpoints split() noexcept
    {
        return Endpoints{Producer(this), Consumer(this)};
    }

private:

    static constexpr size_t kStoredStaticCapacity =
        kUsesStaticCapacity ? Capacity : 0;
    using SlotStorage = std::conditional_t<
        kUsesStaticCapacity,
        std::array<Slot, kStoredStaticCapacity>,
        std::unique_ptr<Slot[]>>;

    [[nodiscard]] static size_t normalizeCapacity(size_t capacity) noexcept
    {
        if (kMaximumCapacity < 2) {
            return 0;
        }
        if (capacity <= 2) {
            return 2;
        }
        if (capacity > kMaximumCapacity) {
            return 0;
        }
        return std::bit_ceil(capacity);
    }

    void initializeLocalStorage(Slot* slots, size_t capacity) noexcept
    {
        m_producerLocal = {slots, capacity, capacity - 1};
        m_consumerLocal = {slots, capacity, capacity - 1};
    }

    [[nodiscard]] static size_t cursorDistance(Cursor newer, Cursor older) noexcept
    {
        return static_cast<size_t>(static_cast<Cursor>(newer - older));
    }

    // 两条共享发布线和两条单侧本地线完全隔离，避免对端轮询驱逐本地游标。
    PublishedCursor m_tail;
    PublishedCursor m_head;
    LocalCursor m_producerLocal;
    LocalCursor m_consumerLocal;
    SlotStorage m_slots;
    TypeRingBufferError m_error = TypeRingBufferError::kNone;
};

/**
 * @brief 运行时容量、构造时分配槽位的 typed SPSC ring。
 * @tparam T 元素类型，必须满足 TypeRingBufferValue。
 * @tparam Cursor 单调无符号游标类型；默认 size_t。
 */
template <TypeRingBufferValue T,
          TypeRingBufferCursor Cursor = size_t>
using DynamicTypeRingBuffer =
    TypeRingBuffer<T, std::dynamic_extent, Cursor>;

/**
 * @brief 编译期容量、成员内持有槽位的 typed SPSC ring。
 * @tparam T 元素类型，必须满足 TypeRingBufferValue。
 * @tparam Capacity 容量，必须是不小于 2 且处于 Cursor 安全半区间的 2 次幂。
 * @tparam Cursor 单调无符号游标类型；默认 size_t。
 * @note 该类型只能默认构造，构造和稳定数据面均不分配内存。
 * @note 槽位直接属于 ring 对象；大 Capacity 或大 T 会显著增大对象，不应放入
 *       小线程栈或 coroutine frame，应由具有足够存储空间的长生命周期对象持有。
 */
template <TypeRingBufferValue T,
          size_t Capacity,
          TypeRingBufferCursor Cursor = size_t>
    requires (Capacity != std::dynamic_extent &&
              detail::ValidTypeRingBufferCapacity<Capacity, Cursor>)
using StaticTypeRingBuffer = TypeRingBuffer<T, Capacity, Cursor>;

} // namespace galay::utils

#endif // GALAY_UTILS_CACHE_TYPE_RING_BUFFER_HPP
