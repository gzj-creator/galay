#ifndef GALAY_RING_BUFFER_HPP
#define GALAY_RING_BUFFER_HPP

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <cstring>
#include <stdexcept>


namespace galay
{
    template<typename T>
    class Buffer
    {

    };

    template<typename T>
    class RingBuffer {
    private:
        std::unique_ptr<T[]> m_buffer;
        size_t m_capacity;
        size_t m_head = 0;
        size_t m_tail = 0;
        size_t m_count = 0;

        // 计算下一个位置，使用位运算优化取模操作（如果容量是2的幂）
        size_t nextPosition(size_t pos) const {
            if constexpr (usePowerOfTwoOptimization()) {
                return (pos + 1) & (m_capacity - 1);
            } else {
                return (pos + 1) % m_capacity;
            }
        }

        // 检查容量是否为2的幂，以启用位运算优化
        static constexpr bool usePowerOfTwoOptimization() {
            return std::has_single_bit(m_capacity);
        }

    public:
        explicit RingBuffer(size_t capacity) 
            : m_capacity(capacity), m_buffer(std::make_unique<T[]>(capacity)) {
            if (capacity == 0) {
                throw std::invalid_argument("Capacity must be greater than zero");
            }
        }

        // 禁止拷贝
        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;

        // 支持移动
        RingBuffer(RingBuffer&& other) noexcept
            : m_buffer(std::move(other.m_buffer))
            , m_capacity(other.m_capacity)
            , m_head(other.m_head)
            , m_tail(other.m_tail)
            , m_count(other.m_count) {
            other.m_capacity = 0;
            other.m_head = 0;
            other.m_tail = 0;
            other.m_count = 0;
        }

        RingBuffer& operator=(RingBuffer&& other) noexcept {
            if (this != &other) {
                m_buffer = std::move(other.m_buffer);
                m_capacity = other.m_capacity;
                m_head = other.m_head;
                m_tail = other.m_tail;
                m_count = other.m_count;
                
                other.m_capacity = 0;
                other.m_head = 0;
                other.m_tail = 0;
                other.m_count = 0;
            }
            return *this;
        }

        // 放入元素（拷贝语义）
        bool push(const T& value) {
            if (full()) {
                return false;
            }
            
            m_buffer[m_tail] = value;
            m_tail = nextPosition(m_tail);
            ++m_count;
            return true;
        }

        // 放入元素（移动语义）
        bool push(T&& value) {
            if (full()) {
                return false;
            }
            
            m_buffer[m_tail] = std::move(value);
            m_tail = nextPosition(m_tail);
            ++m_count;
            return true;
        }

        // 原地构造元素
        template<typename... Args>
        bool emplace(Args&&... args) {
            if (full()) {
                return false;
            }
            
            new (&m_buffer[m_tail]) T(std::forward<Args>(args)...);
            m_tail = nextPosition(m_tail);
            ++m_count;
            return true;
        }

        // 取出元素
        std::optional<T> pop() {
            if (empty()) {
                return std::nullopt;
            }
            
            T value = std::move(m_buffer[m_head]);
            
            // 对于非平凡类型，需要调用析构函数
            if constexpr (!std::is_trivially_destructible_v<T>) {
                m_buffer[m_head].~T();
            }
            
            m_head = nextPosition(m_head);
            --m_count;
            return value;
        }

        // 查看但不取出元素
        std::optional<T> peek() const {
            if (empty()) {
                return std::nullopt;
            }
            return m_buffer[m_head];
        }

        // 查看指定位置的元素（不取出）
        std::optional<T> peekAt(size_t index) const {
            if (index >= m_count) {
                return std::nullopt;
            }
            size_t pos = (m_head + index) % m_capacity;
            return m_buffer[pos];
        }

        // 批量写入数据
        size_t write(const T* data, size_t count) {
            if (count == 0) {
                return 0;
            }
            
            size_t freeSpace = m_capacity - m_count;
            size_t toWrite = std::min(count, freeSpace);
            
            if (toWrite == 0) {
                return 0;
            }
            
            // 计算连续写入空间
            size_t firstChunk = std::min(toWrite, m_capacity - m_tail);
            std::memcpy(m_buffer.get() + m_tail, data, firstChunk * sizeof(T));
            
            if (firstChunk < toWrite) {
                std::memcpy(m_buffer.get(), data + firstChunk, (toWrite - firstChunk) * sizeof(T));
            }
            
            m_tail = (m_tail + toWrite) % m_capacity;
            m_count += toWrite;
            return toWrite;
        }

        // 批量读取数据
        size_t read(T* dest, size_t count) {
            if (count == 0) {
                return 0;
            }
            
            size_t toRead = std::min(count, m_count);
            
            if (toRead == 0) {
                return 0;
            }
            
            // 计算连续读取空间
            size_t firstChunk = std::min(toRead, m_capacity - m_head);
            std::memcpy(dest, m_buffer.get() + m_head, firstChunk * sizeof(T));
            
            if (firstChunk < toRead) {
                std::memcpy(dest + firstChunk, m_buffer.get(), (toRead - firstChunk) * sizeof(T));
            }
            
            // 对于非平凡类型，需要调用析构函数
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (size_t i = 0; i < toRead; ++i) {
                    size_t pos = (m_head + i) % m_capacity;
                    m_buffer[pos].~T();
                }
            }
            
            m_head = (m_head + toRead) % m_capacity;
            m_count -= toRead;
            return toRead;
        }

        // 直接访问底层数据（危险但高效）
        T* data() noexcept {
            return m_buffer.get();
        }

        const T* data() const noexcept {
            return m_buffer.get();
        }

        // 获取可写入的连续空间
        std::pair<T*, size_t> getWriteRegion() noexcept {
            if (full()) {
                return {nullptr, 0};
            }
            
            size_t contiguousSpace = (m_tail >= m_head) ? 
                (m_capacity - m_tail) : 
                (m_head - m_tail);
            
            return {m_buffer.get() + m_tail, contiguousSpace};
        }

        // 提交已写入的数据
        void commitWrite(size_t count) noexcept {
            if (count == 0) {
                return;
            }
            
            m_tail = (m_tail + count) % m_capacity;
            m_count += count;
        }

        // 获取可读取的连续空间
        std::pair<const T*, size_t> getReadRegion() const noexcept {
            if (empty()) {
                return {nullptr, 0};
            }
            
            size_t contiguousData = (m_head < m_tail) ? 
                (m_tail - m_head) : 
                (m_capacity - m_head);
            
            return {m_buffer.get() + m_head, contiguousData};
        }

        // 提交已读取的数据
        void commitRead(size_t count) noexcept {
            if (count == 0) {
                return;
            }
            
            // 对于非平凡类型，需要调用析构函数
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (size_t i = 0; i < count; ++i) {
                    size_t pos = (m_head + i) % m_capacity;
                    m_buffer[pos].~T();
                }
            }
            
            m_head = (m_head + count) % m_capacity;
            m_count -= count;
        }

        // 容量相关方法
        size_t capacity() const noexcept {
            return m_capacity;
        }

        size_t size() const noexcept {
            return m_count;
        }

        bool empty() const noexcept {
            return m_count == 0;
        }

        bool full() const noexcept {
            return m_count == m_capacity;
        }

        size_t freeSpace() const noexcept {
            return m_capacity - m_count;
        }

        // 清空缓冲区
        void clear() {
            // 对于非平凡类型，需要调用析构函数
            if constexpr (!std::is_trivially_destructible_v<T>) {
                while (!empty()) {
                    m_buffer[m_head].~T();
                    m_head = nextPosition(m_head);
                    --m_count;
                }
            } else {
                m_head = 0;
                m_tail = 0;
                m_count = 0;
            }
        }

        // 迭代器支持（简化版）
        T* begin() noexcept {
            return m_buffer.get() + m_head;
        }

        const T* begin() const noexcept {
            return m_buffer.get() + m_head;
        }

        T* end() noexcept {
            return m_buffer.get() + m_tail;
        }

        const T* end() const noexcept {
            return m_buffer.get() + m_tail;
        }
};
}

#endif