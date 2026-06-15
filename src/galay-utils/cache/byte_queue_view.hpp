/**
 * @file byte_queue_view.hpp
 * @brief 仅追加字节队列视图
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供适合协议解析和流式解码的连续字节队列。数据尾部追加、
 *          头部消费，并在已消费区域较大时原地压缩。非线程安全。
 */

#ifndef GALAY_UTILS_CACHE_BYTE_QUEUE_VIEW_HPP
#define GALAY_UTILS_CACHE_BYTE_QUEUE_VIEW_HPP

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace galay::utils {

/**
 * @brief 仅追加字节队列视图
 * @details 在连续 vector 存储中维护读偏移，避免每次 consume 都移动数据。
 *          当已消费区域达到压缩阈值时，将剩余可读数据移动到存储起点。
 *
 * @warning 本类不提供线程安全保证。并发访问时调用方必须在外部同步。
 */
class ByteQueueView {
public:
    /**
     * @brief 默认构造空队列
     */
    ByteQueueView() = default;

    /**
     * @brief 以预留容量构造队列
     * @param reserveSize 预留字节数
     */
    explicit ByteQueueView(size_t reserveSize) {
        reserve(reserveSize);
    }

    /**
     * @brief 预留底层存储容量
     * @param capacity 至少预留的字节数
     */
    void reserve(size_t capacity) {
        m_storage.reserve(capacity);
    }

    /**
     * @brief 追加原始字节
     * @param data 字节指针；为空时本次追加为空操作
     * @param length 字节数
     */
    void append(const char* data, size_t length) {
        if (data == nullptr || length == 0) {
            return;
        }
        if (m_readOffset == m_storage.size()) {
            clear();
        }
        m_storage.insert(m_storage.end(), data, data + length);
    }

    /**
     * @brief 追加字符串视图中的字节
     * @param bytes 字节视图
     */
    void append(std::string_view bytes) {
        append(bytes.data(), bytes.size());
    }

    /**
     * @brief 追加 std::byte 视图中的字节
     * @param bytes 字节视图
     */
    void append(std::span<const std::byte> bytes) {
        append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    /**
     * @brief 获取可读字节数
     * @return 当前可读字节数
     */
    [[nodiscard]] size_t size() const noexcept {
        return m_storage.size() - m_readOffset;
    }

    /**
     * @brief 判断队列是否为空
     * @return 无可读字节返回 true
     */
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    /**
     * @brief 判断是否至少有指定字节数可读
     * @param length 所需字节数
     * @return 可读字节数足够返回 true
     */
    [[nodiscard]] bool has(size_t length) const noexcept {
        return size() >= length;
    }

    /**
     * @brief 获取第一个可读字节指针
     * @return 指向可读区域起点的指针；为空时返回 nullptr
     */
    [[nodiscard]] const char* data() const noexcept {
        if (empty()) {
            return nullptr;
        }
        return m_storage.data() + m_readOffset;
    }

    /**
     * @brief 获取可读数据的子视图
     * @param offset 从当前读位置开始的偏移
     * @param length 视图长度
     * @return 范围合法时返回视图，否则返回空视图
     */
    [[nodiscard]] std::string_view view(size_t offset, size_t length) const noexcept {
        if (offset > size() || length > size() - offset) {
            return {};
        }
        if (length == 0) {
            return {};
        }
        return std::string_view(data() + offset, length);
    }

    /**
     * @brief 消费头部字节
     * @param length 要消费的字节数；大于等于 size() 时清空队列
     */
    void consume(size_t length) {
        if (length >= size()) {
            clear();
            return;
        }
        m_readOffset += length;
        compactIfNeeded();
    }

    /**
     * @brief 清空队列
     */
    void clear() noexcept {
        m_storage.clear();
        m_readOffset = 0;
    }

private:
    void compactIfNeeded() {
        const size_t readable = size();
        if (m_readOffset == 0) {
            return;
        }
        if (readable == 0) {
            clear();
            return;
        }
        if (m_readOffset < kCompactOffsetThreshold && m_readOffset * 2 < m_storage.size()) {
            return;
        }

        std::memmove(m_storage.data(), m_storage.data() + m_readOffset, readable);
        m_storage.resize(readable);
        m_readOffset = 0;
    }

    static constexpr size_t kCompactOffsetThreshold = 4096;

    std::vector<char> m_storage;
    size_t m_readOffset = 0;
};

} // namespace galay::utils

#endif // GALAY_UTILS_CACHE_BYTE_QUEUE_VIEW_HPP
