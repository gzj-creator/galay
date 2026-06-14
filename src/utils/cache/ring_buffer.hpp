/**
 * @file ring_buffer.hpp
 * @brief 固定容量环形缓冲区
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供跨平台、固定容量、move-only 的字节环形缓冲区。
 *          核心接口使用 span；在 POSIX 平台额外提供 iovec 适配。
 */

#ifndef GALAY_UTILS_CACHE_RING_BUFFER_HPP
#define GALAY_UTILS_CACHE_RING_BUFFER_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/uio.h>
#define GALAY_UTILS_RING_BUFFER_HAS_IOVEC 1
#else
#define GALAY_UTILS_RING_BUFFER_HAS_IOVEC 0
#endif

namespace galay::utils {

/**
 * @brief 固定容量环形缓冲区
 * @details 支持环绕读写、scatter/gather 风格的 span 视图，以及显式
 *          produce()/consume() 指针推进。非线程安全。
 *
 * @warning 本类不提供线程安全保证。并发访问时调用方必须在外部同步。
 */
class RingBuffer {
public:
    static constexpr size_t kDefaultCapacity = 4096; ///< 默认容量

    /**
     * @brief 构造固定容量环形缓冲区
     * @param capacity 缓冲区容量，必须大于 0
     * @throws std::invalid_argument capacity 为 0 时抛出
     */
    explicit RingBuffer(size_t capacity = kDefaultCapacity)
        : m_buffer(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /**
     * @brief 移动构造环形缓冲区
     * @param other 源对象；移动后保持空状态，可安全查询或重新赋值
     */
    RingBuffer(RingBuffer&& other) noexcept
        : m_buffer(std::move(other.m_buffer))
        , m_readIndex(std::exchange(other.m_readIndex, 0))
        , m_writeIndex(std::exchange(other.m_writeIndex, 0))
        , m_size(std::exchange(other.m_size, 0)) {}

    /**
     * @brief 移动赋值环形缓冲区
     * @param other 源对象；移动后保持空状态，可安全查询或重新赋值
     * @return 当前对象引用
     */
    RingBuffer& operator=(RingBuffer&& other) noexcept {
        if (this != &other) {
            m_buffer = std::move(other.m_buffer);
            m_readIndex = std::exchange(other.m_readIndex, 0);
            m_writeIndex = std::exchange(other.m_writeIndex, 0);
            m_size = std::exchange(other.m_size, 0);
        }
        return *this;
    }

    /**
     * @brief 获取可读字节数
     * @return 可读字节数
     */
    size_t readable() const noexcept {
        return m_size;
    }

    /**
     * @brief 获取可写字节数
     * @return 可写字节数
     */
    size_t writable() const noexcept {
        return capacity() - m_size;
    }

    /**
     * @brief 获取固定容量
     * @return 缓冲区容量
     */
    size_t capacity() const noexcept {
        return m_buffer.size();
    }

    /**
     * @brief 判断是否无可读数据
     * @return 为空返回 true
     */
    bool empty() const noexcept {
        return m_size == 0;
    }

    /**
     * @brief 判断是否无可写空间
     * @return 已满返回 true
     */
    bool full() const noexcept {
        return m_size == capacity();
    }

    /**
     * @brief 获取可写连续片段
     * @param out 输出数组，最多填充两个 span
     * @return 有效 span 数量
     */
    size_t writeSpans(std::array<std::span<std::byte>, 2>& out) noexcept {
        out = {};
        if (full()) {
            return 0;
        }

        size_t count = 0;
        size_t remaining = writable();
        if (m_writeIndex >= m_readIndex) {
            const size_t first = std::min(remaining, capacity() - m_writeIndex);
            if (first > 0) {
                out[count++] = std::span<std::byte>(m_buffer.data() + m_writeIndex, first);
                remaining -= first;
            }
            if (remaining > 0 && m_readIndex > 0) {
                out[count++] = std::span<std::byte>(m_buffer.data(), std::min(remaining, m_readIndex));
            }
        } else {
            const size_t first = std::min(remaining, m_readIndex - m_writeIndex);
            if (first > 0) {
                out[count++] = std::span<std::byte>(m_buffer.data() + m_writeIndex, first);
            }
        }
        return count;
    }

    /**
     * @brief 获取可读连续片段
     * @param out 输出数组，最多填充两个只读 span
     * @return 有效 span 数量
     */
    size_t readSpans(std::array<std::span<const std::byte>, 2>& out) const noexcept {
        out = {};
        if (empty()) {
            return 0;
        }

        size_t count = 0;
        size_t remaining = readable();
        if (m_readIndex < m_writeIndex) {
            out[count++] = std::span<const std::byte>(m_buffer.data() + m_readIndex, remaining);
        } else {
            const size_t first = std::min(remaining, capacity() - m_readIndex);
            if (first > 0) {
                out[count++] = std::span<const std::byte>(m_buffer.data() + m_readIndex, first);
                remaining -= first;
            }
            if (remaining > 0) {
                out[count++] = std::span<const std::byte>(m_buffer.data(), remaining);
            }
        }
        return count;
    }

#if GALAY_UTILS_RING_BUFFER_HAS_IOVEC
    /**
     * @brief 获取可写区域的 POSIX iovec 描述符
     * @param out 输出 iovec 数组；为空或容量为 0 时返回 0
     * @param maxIovecs 数组容量；最多填充两个条目
     * @return 有效 iovec 数量
     *
     * @note 该接口用于兼容 readv/writev 类 I/O。方法是逻辑 const，
     *       但返回的 iov_base 指向可写内存，调用方应在实际写入后调用 produce()。
     */
    size_t getWriteIovecs(struct iovec* out, size_t maxIovecs = 2) const noexcept {
        if (out == nullptr || maxIovecs == 0 || full()) {
            return 0;
        }

        auto* base = const_cast<std::byte*>(m_buffer.data());
        size_t count = 0;
        size_t remaining = writable();
        if (m_writeIndex >= m_readIndex) {
            const size_t first = std::min(remaining, capacity() - m_writeIndex);
            if (first > 0 && count < maxIovecs) {
                out[count++] = iovec{base + m_writeIndex, first};
                remaining -= first;
            }
            if (remaining > 0 && m_readIndex > 0 && count < maxIovecs) {
                out[count++] = iovec{base, std::min(remaining, m_readIndex)};
            }
        } else if (count < maxIovecs) {
            const size_t first = std::min(remaining, m_readIndex - m_writeIndex);
            if (first > 0) {
                out[count++] = iovec{base + m_writeIndex, first};
            }
        }
        return count;
    }

    /**
     * @brief 获取可写区域的 POSIX iovec 描述符
     * @tparam N 数组容量
     * @param out 输出 iovec 数组
     * @return 有效 iovec 数量
     */
    template<size_t N>
    size_t getWriteIovecs(std::array<struct iovec, N>& out) const noexcept {
        return getWriteIovecs(out.data(), N);
    }

    /**
     * @brief 获取可读区域的 POSIX iovec 描述符
     * @param out 输出 iovec 数组；为空或容量为 0 时返回 0
     * @param maxIovecs 数组容量；最多填充两个条目
     * @return 有效 iovec 数量
     *
     * @note POSIX iovec 的 iov_base 类型为 void*，因此只读区域也以非 const
     *       指针形式返回；调用方用于 writev 时不应修改这段内存。
     */
    size_t getReadIovecs(struct iovec* out, size_t maxIovecs = 2) const noexcept {
        if (out == nullptr || maxIovecs == 0 || empty()) {
            return 0;
        }

        auto* base = const_cast<std::byte*>(m_buffer.data());
        size_t count = 0;
        size_t remaining = readable();
        if (m_readIndex < m_writeIndex) {
            out[count++] = iovec{base + m_readIndex, remaining};
        } else {
            const size_t first = std::min(remaining, capacity() - m_readIndex);
            if (first > 0 && count < maxIovecs) {
                out[count++] = iovec{base + m_readIndex, first};
                remaining -= first;
            }
            if (remaining > 0 && count < maxIovecs) {
                out[count++] = iovec{base, remaining};
            }
        }
        return count;
    }

    /**
     * @brief 获取可读区域的 POSIX iovec 描述符
     * @tparam N 数组容量
     * @param out 输出 iovec 数组
     * @return 有效 iovec 数量
     */
    template<size_t N>
    size_t getReadIovecs(std::array<struct iovec, N>& out) const noexcept {
        return getReadIovecs(out.data(), N);
    }
#endif

    /**
     * @brief 确认外部已经写入的字节数并推进写指针
     * @param length 已写入字节数；超过 writable() 时自动截断
     */
    void produce(size_t length) noexcept {
        if (length == 0 || capacity() == 0) {
            return;
        }
        const size_t actual = std::min(length, writable());
        m_writeIndex = (m_writeIndex + actual) % capacity();
        m_size += actual;
    }

    /**
     * @brief 消费头部字节并推进读指针
     * @param length 要消费的字节数；超过 readable() 时自动截断
     */
    void consume(size_t length) noexcept {
        if (length == 0 || capacity() == 0) {
            return;
        }
        const size_t actual = std::min(length, readable());
        m_readIndex = (m_readIndex + actual) % capacity();
        m_size -= actual;
        if (m_size == 0) {
            m_readIndex = 0;
            m_writeIndex = 0;
        }
    }

    /**
     * @brief 清空缓冲区但保留容量
     */
    void clear() noexcept {
        m_readIndex = 0;
        m_writeIndex = 0;
        m_size = 0;
    }

    /**
     * @brief 写入原始字节
     * @param data 源数据指针；length 为 0 时可以为 nullptr
     * @param length 请求写入字节数
     * @return 实际写入字节数
     */
    size_t write(const void* data, size_t length) {
        if (data == nullptr || length == 0 || full()) {
            return 0;
        }

        std::array<std::span<std::byte>, 2> spans{};
        const size_t count = writeSpans(spans);
        const auto* source = static_cast<const std::byte*>(data);
        size_t written = 0;
        const size_t toWrite = std::min(length, writable());

        for (size_t i = 0; i < count && written < toWrite; ++i) {
            const size_t chunk = std::min(spans[i].size(), toWrite - written);
            std::memcpy(spans[i].data(), source + written, chunk);
            written += chunk;
        }

        produce(written);
        return written;
    }

    /**
     * @brief 写入字符串视图中的字节
     * @param bytes 字节视图
     * @return 实际写入字节数
     */
    size_t write(std::string_view bytes) {
        return write(bytes.data(), bytes.size());
    }

    /**
     * @brief 读取字节到目标缓冲区
     * @param data 目标指针；length 为 0 时可以为 nullptr
     * @param length 请求读取字节数
     * @return 实际读取字节数
     */
    size_t read(void* data, size_t length) {
        if (data == nullptr || length == 0 || empty()) {
            return 0;
        }

        std::array<std::span<const std::byte>, 2> spans{};
        const size_t count = readSpans(spans);
        auto* target = static_cast<std::byte*>(data);
        size_t readBytes = 0;
        const size_t toRead = std::min(length, readable());

        for (size_t i = 0; i < count && readBytes < toRead; ++i) {
            const size_t chunk = std::min(spans[i].size(), toRead - readBytes);
            std::memcpy(target + readBytes, spans[i].data(), chunk);
            readBytes += chunk;
        }

        consume(readBytes);
        return readBytes;
    }

private:
    std::vector<std::byte> m_buffer;
    size_t m_readIndex = 0;
    size_t m_writeIndex = 0;
    size_t m_size = 0;
};

} // namespace galay::utils

#undef GALAY_UTILS_RING_BUFFER_HAS_IOVEC

#endif // GALAY_UTILS_CACHE_RING_BUFFER_HPP
