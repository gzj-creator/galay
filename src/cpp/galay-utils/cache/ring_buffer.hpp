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
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#define GALAY_UTILS_RING_BUFFER_HAS_IOVEC 1
#define GALAY_UTILS_RING_BUFFER_HAS_MMAP 1
#else
#define GALAY_UTILS_RING_BUFFER_HAS_IOVEC 0
#define GALAY_UTILS_RING_BUFFER_HAS_MMAP 0
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

#if GALAY_UTILS_RING_BUFFER_HAS_MMAP && !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace galay::utils {

/**
 * @brief RingBuffer 后端选择策略
 * @details 用作 RingBuffer 模板参数；Mmap 提供双映射单段视图，
 *          Vector 保留物理环绕的双段视图，Auto 按容量运行时选择。
 */
enum class RingBufferBackendStrategy {
    Mmap,   ///< 双映射 mmap 后端，跨环绕边界仍返回单段连续视图
    Vector, ///< vector 后端，环绕时最多返回两段视图
    Auto    ///< 自动选择：大容量优先 mmap，小容量使用 vector
};

/**
 * @brief RingBuffer 内部资源创建错误
 * @details 仅用于 mmap 双映射实现的工厂结果。公开 RingBuffer 构造在
 *          mmap 失败时会自动降级到 vector 实现，保持既有可用性。
 */
enum class RingBufferError {
    kOk = 0,                  ///< 无错误
    kInvalidCapacity,         ///< 容量为 0 或页对齐后溢出
    kSharedMemoryCreateFail,  ///< 创建匿名共享内存 fd 失败
    kResizeFail,              ///< 调整共享内存大小失败
    kAddressReserveFail,      ///< 保留连续虚拟地址失败
    kMappingFail,             ///< 双映射共享内存失败
};

/**
 * @brief 获取 RingBufferError 的静态错误字符串
 * @param error 错误枚举
 * @return 覆盖所有公开枚举值的非空字符串
 */
[[nodiscard]] inline const char* ringBufferErrorString(RingBufferError error) noexcept {
    switch (error) {
    case RingBufferError::kOk:
        return "ok";
    case RingBufferError::kInvalidCapacity:
        return "invalid capacity";
    case RingBufferError::kSharedMemoryCreateFail:
        return "shared memory create failed";
    case RingBufferError::kResizeFail:
        return "shared memory resize failed";
    case RingBufferError::kAddressReserveFail:
        return "address reserve failed";
    case RingBufferError::kMappingFail:
        return "mapping failed";
    }
    return "unknown ring buffer error";
}

namespace detail {

#if GALAY_UTILS_RING_BUFFER_HAS_MMAP

[[nodiscard]] inline std::expected<size_t, RingBufferError>
alignRingBufferCapacity(size_t capacity) noexcept {
    if (capacity == 0) {
        return std::unexpected(RingBufferError::kInvalidCapacity);
    }

    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return std::unexpected(RingBufferError::kAddressReserveFail);
    }

    const size_t page = static_cast<size_t>(page_size);
    if (capacity > std::numeric_limits<size_t>::max() - (page - 1)) {
        return std::unexpected(RingBufferError::kInvalidCapacity);
    }

    const size_t aligned = ((capacity + page - 1) / page) * page;
    if (aligned == 0 || aligned > std::numeric_limits<size_t>::max() / 2) {
        return std::unexpected(RingBufferError::kInvalidCapacity);
    }
    return aligned;
}

inline void closeDescriptorNoexcept(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    const int close_result = ::close(fd);
    if (close_result != 0) {
        // noexcept cleanup has no error channel; callers already receive the primary failure.
    }
}

inline void unmapNoexcept(void* address, size_t length) noexcept {
    if (address == nullptr || length == 0) {
        return;
    }
    const int unmap_result = ::munmap(address, length);
    if (unmap_result != 0) {
        // noexcept cleanup has no error channel; the mapping is no longer used by this object.
    }
}

[[nodiscard]] inline int createRingBufferBackingFile() noexcept {
#if defined(__linux__)
#if defined(SYS_memfd_create)
    const long memfd = ::syscall(SYS_memfd_create, "galay_ring_buffer", MFD_CLOEXEC);
    if (memfd >= 0) {
        return static_cast<int>(memfd);
    }
#endif
    return -1;
#else

    static std::atomic_uint64_t counter{0};
    for (int attempt = 0; attempt < 16; ++attempt) {
        char name[96]{};
        const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
        const int written = std::snprintf(name, sizeof(name), "/galay_ring_%ld_%llu",
                                          static_cast<long>(::getpid()),
                                          static_cast<unsigned long long>(sequence));
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(name)) {
            return -1;
        }

        int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const int fd = ::shm_open(name, flags, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            const int unlink_result = ::shm_unlink(name);
            if (unlink_result != 0) {
                closeDescriptorNoexcept(fd);
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    return -1;
#endif
}

#endif // GALAY_UTILS_RING_BUFFER_HAS_MMAP

/**
 * @brief vector 后端环形缓冲实现
 * @details 保留原 RingBuffer 的物理环绕逻辑；小容量缓冲区使用该实现，
 *          跨边界时 span/iovec 最多拆为两个片段。非线程安全。
 */
class VectorRingBufferImpl {
public:
    explicit VectorRingBufferImpl(size_t capacity)
        : m_buffer(capacity) {}

    VectorRingBufferImpl(const VectorRingBufferImpl&) = delete;
    VectorRingBufferImpl& operator=(const VectorRingBufferImpl&) = delete;

    VectorRingBufferImpl(VectorRingBufferImpl&& other) noexcept
        : m_buffer(std::move(other.m_buffer))
        , m_readIndex(std::exchange(other.m_readIndex, 0))
        , m_writeIndex(std::exchange(other.m_writeIndex, 0))
        , m_size(std::exchange(other.m_size, 0)) {}

    VectorRingBufferImpl& operator=(VectorRingBufferImpl&& other) noexcept {
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
        std::array<Segment, 2> segments{};
        const size_t count = writeSegments(segments);
        for (size_t i = 0; i < count; ++i) {
            out[i] = std::span<std::byte>(m_buffer.data() + segments[i].first, segments[i].second);
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
        std::array<Segment, 2> segments{};
        const size_t count = readSegments(segments);
        for (size_t i = 0; i < count; ++i) {
            out[i] = std::span<const std::byte>(m_buffer.data() + segments[i].first, segments[i].second);
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
        if (out == nullptr || maxIovecs == 0) {
            return 0;
        }

        std::array<Segment, 2> segments{};
        const size_t count = std::min(writeSegments(segments), maxIovecs);
        auto* base = const_cast<std::byte*>(m_buffer.data());
        for (size_t i = 0; i < count; ++i) {
            out[i] = iovec{base + segments[i].first, segments[i].second};
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
        if (out == nullptr || maxIovecs == 0) {
            return 0;
        }

        std::array<Segment, 2> segments{};
        const size_t count = std::min(readSegments(segments), maxIovecs);
        auto* base = const_cast<std::byte*>(m_buffer.data());
        for (size_t i = 0; i < count; ++i) {
            out[i] = iovec{base + segments[i].first, segments[i].second};
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
    /// 环形可用区段的 {偏移, 长度}；环绕时最多两段。
    using Segment = std::pair<size_t, size_t>;

    /**
     * @brief 计算可写区域的环形区段（单一事实来源）
     * @param seg 输出区段数组，最多填充两段
     * @return 有效区段数量；已满时返回 0
     */
    size_t writeSegments(std::array<Segment, 2>& seg) const noexcept {
        if (full()) {
            return 0;
        }
        size_t count = 0;
        size_t remaining = writable();
        if (m_writeIndex >= m_readIndex) {
            const size_t first = std::min(remaining, capacity() - m_writeIndex);
            if (first > 0) {
                seg[count++] = {m_writeIndex, first};
                remaining -= first;
            }
            if (remaining > 0 && m_readIndex > 0) {
                seg[count++] = {0, std::min(remaining, m_readIndex)};
            }
        } else {
            const size_t first = std::min(remaining, m_readIndex - m_writeIndex);
            if (first > 0) {
                seg[count++] = {m_writeIndex, first};
            }
        }
        return count;
    }

    /**
     * @brief 计算可读区域的环形区段（单一事实来源）
     * @param seg 输出区段数组，最多填充两段
     * @return 有效区段数量；为空时返回 0
     */
    size_t readSegments(std::array<Segment, 2>& seg) const noexcept {
        if (empty()) {
            return 0;
        }
        size_t count = 0;
        size_t remaining = readable();
        if (m_readIndex < m_writeIndex) {
            seg[count++] = {m_readIndex, remaining};
        } else {
            const size_t first = std::min(remaining, capacity() - m_readIndex);
            if (first > 0) {
                seg[count++] = {m_readIndex, first};
                remaining -= first;
            }
            if (remaining > 0) {
                seg[count++] = {0, remaining};
            }
        }
        return count;
    }

    std::vector<std::byte> m_buffer;
    size_t m_readIndex = 0;
    size_t m_writeIndex = 0;
    size_t m_size = 0;
};

#if GALAY_UTILS_RING_BUFFER_HAS_MMAP

/**
 * @brief mmap 双映射后端环形缓冲实现
 * @details 将同一段共享内存映射到连续的两段虚拟地址，使跨环绕边界的
 *          可读/可写区域仍呈现为单段连续 span/iovec。非线程安全。
 */
class MmapRingBufferImpl {
public:
    /**
     * @brief 创建 mmap 双映射环形缓冲
     * @param capacity 请求容量；会向上按系统页大小对齐
     * @return 成功返回实现对象，失败返回可诊断的 RingBufferError
     */
    [[nodiscard]] static std::expected<MmapRingBufferImpl, RingBufferError>
    create(size_t capacity) noexcept {
        auto aligned_capacity = alignRingBufferCapacity(capacity);
        if (!aligned_capacity) {
            return std::unexpected(aligned_capacity.error());
        }

        const int fd = createRingBufferBackingFile();
        if (fd < 0) {
            return std::unexpected(RingBufferError::kSharedMemoryCreateFail);
        }

        const auto off_max = static_cast<size_t>(std::numeric_limits<off_t>::max());
        if (*aligned_capacity > off_max) {
            closeDescriptorNoexcept(fd);
            return std::unexpected(RingBufferError::kResizeFail);
        }

        const int resize_result = ::ftruncate(fd, static_cast<off_t>(*aligned_capacity));
        if (resize_result != 0) {
            closeDescriptorNoexcept(fd);
            return std::unexpected(RingBufferError::kResizeFail);
        }

        const size_t virtual_size = *aligned_capacity * 2;
        void* reserved = ::mmap(nullptr, virtual_size, PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserved == MAP_FAILED) {
            closeDescriptorNoexcept(fd);
            return std::unexpected(RingBufferError::kAddressReserveFail);
        }

        void* first = ::mmap(reserved, *aligned_capacity, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_FIXED, fd, 0);
        void* second = ::mmap(static_cast<std::byte*>(reserved) + *aligned_capacity,
                              *aligned_capacity, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, fd, 0);
        if (first == MAP_FAILED || second == MAP_FAILED) {
            unmapNoexcept(reserved, virtual_size);
            closeDescriptorNoexcept(fd);
            return std::unexpected(RingBufferError::kMappingFail);
        }

        return MmapRingBufferImpl(static_cast<std::byte*>(reserved), *aligned_capacity, fd);
    }

    MmapRingBufferImpl(const MmapRingBufferImpl&) = delete;
    MmapRingBufferImpl& operator=(const MmapRingBufferImpl&) = delete;

    MmapRingBufferImpl(MmapRingBufferImpl&& other) noexcept
        : m_base(std::exchange(other.m_base, nullptr))
        , m_capacity(std::exchange(other.m_capacity, 0))
        , m_readIndex(std::exchange(other.m_readIndex, 0))
        , m_writeIndex(std::exchange(other.m_writeIndex, 0))
        , m_size(std::exchange(other.m_size, 0))
        , m_fd(std::exchange(other.m_fd, -1)) {}

    MmapRingBufferImpl& operator=(MmapRingBufferImpl&& other) noexcept {
        if (this != &other) {
            reset();
            m_base = std::exchange(other.m_base, nullptr);
            m_capacity = std::exchange(other.m_capacity, 0);
            m_readIndex = std::exchange(other.m_readIndex, 0);
            m_writeIndex = std::exchange(other.m_writeIndex, 0);
            m_size = std::exchange(other.m_size, 0);
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }

    ~MmapRingBufferImpl() noexcept {
        reset();
    }

    size_t readable() const noexcept {
        return m_size;
    }

    size_t writable() const noexcept {
        return capacity() - m_size;
    }

    size_t capacity() const noexcept {
        return m_capacity;
    }

    bool empty() const noexcept {
        return m_size == 0;
    }

    bool full() const noexcept {
        return m_size == capacity();
    }

    size_t writeSpans(std::array<std::span<std::byte>, 2>& out) noexcept {
        out = {};
        if (full()) {
            return 0;
        }
        out[0] = std::span<std::byte>(m_base + m_writeIndex, writable());
        return 1;
    }

    size_t readSpans(std::array<std::span<const std::byte>, 2>& out) const noexcept {
        out = {};
        if (empty()) {
            return 0;
        }
        out[0] = std::span<const std::byte>(m_base + m_readIndex, readable());
        return 1;
    }

#if GALAY_UTILS_RING_BUFFER_HAS_IOVEC
    size_t getWriteIovecs(struct iovec* out, size_t maxIovecs = 2) const noexcept {
        if (out == nullptr || maxIovecs == 0 || full()) {
            return 0;
        }
        out[0] = iovec{m_base + m_writeIndex, writable()};
        return 1;
    }

    template<size_t N>
    size_t getWriteIovecs(std::array<struct iovec, N>& out) const noexcept {
        return getWriteIovecs(out.data(), N);
    }

    size_t getReadIovecs(struct iovec* out, size_t maxIovecs = 2) const noexcept {
        if (out == nullptr || maxIovecs == 0 || empty()) {
            return 0;
        }
        out[0] = iovec{m_base + m_readIndex, readable()};
        return 1;
    }

    template<size_t N>
    size_t getReadIovecs(std::array<struct iovec, N>& out) const noexcept {
        return getReadIovecs(out.data(), N);
    }
#endif

    void produce(size_t length) noexcept {
        if (length == 0 || capacity() == 0) {
            return;
        }
        const size_t actual = std::min(length, writable());
        m_writeIndex += actual;
        if (m_writeIndex >= capacity()) {
            m_writeIndex -= capacity();
        }
        m_size += actual;
    }

    void consume(size_t length) noexcept {
        if (length == 0 || capacity() == 0) {
            return;
        }
        const size_t actual = std::min(length, readable());
        m_readIndex += actual;
        if (m_readIndex >= capacity()) {
            m_readIndex -= capacity();
        }
        m_size -= actual;
        if (m_size == 0) {
            m_readIndex = 0;
            m_writeIndex = 0;
        }
    }

    void clear() noexcept {
        m_readIndex = 0;
        m_writeIndex = 0;
        m_size = 0;
    }

    size_t write(const void* data, size_t length) {
        if (data == nullptr || length == 0 || full()) {
            return 0;
        }

        const size_t written = std::min(length, writable());
        std::memcpy(m_base + m_writeIndex, data, written);
        produce(written);
        return written;
    }

    size_t write(std::string_view bytes) {
        return write(bytes.data(), bytes.size());
    }

    size_t read(void* data, size_t length) {
        if (data == nullptr || length == 0 || empty()) {
            return 0;
        }

        const size_t read_bytes = std::min(length, readable());
        std::memcpy(data, m_base + m_readIndex, read_bytes);
        consume(read_bytes);
        return read_bytes;
    }

private:
    MmapRingBufferImpl(std::byte* base, size_t capacity, int fd) noexcept
        : m_base(base)
        , m_capacity(capacity)
        , m_fd(fd) {}

    void reset() noexcept {
        if (m_base != nullptr && m_capacity > 0) {
            unmapNoexcept(m_base, m_capacity * 2);
        }
        closeDescriptorNoexcept(m_fd);
        m_base = nullptr;
        m_capacity = 0;
        m_readIndex = 0;
        m_writeIndex = 0;
        m_size = 0;
        m_fd = -1;
    }

    std::byte* m_base = nullptr;
    size_t m_capacity = 0;
    size_t m_readIndex = 0;
    size_t m_writeIndex = 0;
    size_t m_size = 0;
    int m_fd = -1;
};

#endif // GALAY_UTILS_RING_BUFFER_HAS_MMAP

template<typename T>
struct IsRingBufferVariant : std::false_type {};

template<typename... Types>
struct IsRingBufferVariant<std::variant<Types...>> : std::true_type {};

template<typename Impl, typename Visitor>
decltype(auto) visitRingBufferImpl(Impl&& impl, Visitor&& visitor) {
    if constexpr (IsRingBufferVariant<std::remove_cvref_t<Impl>>::value) {
        return std::visit(std::forward<Visitor>(visitor), std::forward<Impl>(impl));
    } else {
        return std::forward<Visitor>(visitor)(std::forward<Impl>(impl));
    }
}

} // namespace detail

/**
 * @brief 固定容量环形缓冲区
 * @details 通过 RingBufferBackendStrategy 模板参数在编译期选择默认后端：
 *          Mmap 返回单段连续视图，Vector 保留物理环绕双段视图，
 *          Auto 按容量阈值运行时选择。非线程安全。
 *
 * @tparam Strategy 后端策略；默认 Mmap。
 * @warning 本类不提供线程安全保证。并发访问时调用方必须在外部同步。
 */
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RingBuffer {
public:
    static constexpr size_t kDefaultCapacity = 4096;       ///< 默认容量
    static constexpr size_t kMmapThreshold = 64 * 1024;    ///< mmap 后端启用阈值

    /**
     * @brief 构造固定容量环形缓冲区
     * @param capacity 缓冲区容量，必须大于 0；mmap 后端会按页向上对齐
     * @throws std::invalid_argument capacity 为 0 时保留既有抛出行为
     * @note mmap 资源创建失败时自动降级到 vector 后端，不影响功能可用性。
     */
    explicit RingBuffer(size_t capacity = kDefaultCapacity)
        : m_impl(makeImpl(capacity)) {}

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&& other) noexcept = default;
    RingBuffer& operator=(RingBuffer&& other) noexcept = default;

    /**
     * @brief 显式复制当前可读内容到新的独立环形缓冲区
     * @return 容量一致、可读字节序列一致的缓冲区副本
     * @note 复制不保留内部读写索引形状，只保证后续 read() 的字节序列一致。
     */
    [[nodiscard]] RingBuffer clone() const {
        RingBuffer copy(capacity());
        std::array<std::span<const std::byte>, 2> spans{};
        const size_t span_count = readSpans(spans);
        for (size_t index = 0; index < span_count; ++index) {
            const size_t written = copy.write(spans[index].data(), spans[index].size());
            if (written != spans[index].size()) {
                copy.clear();
                return copy;
            }
        }
        return copy;
    }

    /**
     * @brief 获取可读字节数
     * @return 可读字节数
     */
    size_t readable() const noexcept {
        return detail::visitRingBufferImpl(m_impl, [](const auto& impl) { return impl.readable(); });
    }

    /**
     * @brief 获取可写字节数
     * @return 可写字节数
     */
    size_t writable() const noexcept {
        return detail::visitRingBufferImpl(m_impl, [](const auto& impl) { return impl.writable(); });
    }

    /**
     * @brief 获取当前后端容量
     * @return vector 后端返回请求容量；mmap 后端返回页对齐后的实际容量
     */
    size_t capacity() const noexcept {
        return detail::visitRingBufferImpl(m_impl, [](const auto& impl) { return impl.capacity(); });
    }

    /**
     * @brief 判断是否无可读数据
     * @return 为空返回 true
     */
    bool empty() const noexcept {
        return detail::visitRingBufferImpl(m_impl, [](const auto& impl) { return impl.empty(); });
    }

    /**
     * @brief 判断是否无可写空间
     * @return 已满返回 true
     */
    bool full() const noexcept {
        return detail::visitRingBufferImpl(m_impl, [](const auto& impl) { return impl.full(); });
    }

    /**
     * @brief 获取可写连续片段
     * @param out 输出数组，vector 后端最多两个 span，mmap 后端最多一个 span
     * @return 有效 span 数量
     */
    size_t writeSpans(std::array<std::span<std::byte>, 2>& out) noexcept {
        return detail::visitRingBufferImpl(m_impl, [&out](auto& impl) { return impl.writeSpans(out); });
    }

    /**
     * @brief 获取可读连续片段
     * @param out 输出数组，vector 后端最多两个 span，mmap 后端最多一个 span
     * @return 有效 span 数量
     */
    size_t readSpans(std::array<std::span<const std::byte>, 2>& out) const noexcept {
        return detail::visitRingBufferImpl(m_impl, [&out](const auto& impl) { return impl.readSpans(out); });
    }

#if GALAY_UTILS_RING_BUFFER_HAS_IOVEC
    /**
     * @brief 获取可写区域的 POSIX iovec 描述符
     * @param out 输出 iovec 数组；为空或容量为 0 时返回 0
     * @param maxIovecs 数组容量；vector 后端最多两个条目，mmap 后端最多一个条目
     * @return 有效 iovec 数量
     *
     * @note 方法是逻辑 const，但返回的 iov_base 指向可写内存；调用方应在实际写入后调用 produce()。
     */
    size_t getWriteIovecs(struct iovec* out, size_t maxIovecs = 2) const noexcept {
        return detail::visitRingBufferImpl(m_impl, [out, maxIovecs](const auto& impl) {
            return impl.getWriteIovecs(out, maxIovecs);
        });
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
     * @param maxIovecs 数组容量；vector 后端最多两个条目，mmap 后端最多一个条目
     * @return 有效 iovec 数量
     *
     * @note POSIX iovec 的 iov_base 类型为 void*，调用方用于 writev 时不应修改这段内存。
     */
    size_t getReadIovecs(struct iovec* out, size_t maxIovecs = 2) const noexcept {
        return detail::visitRingBufferImpl(m_impl, [out, maxIovecs](const auto& impl) {
            return impl.getReadIovecs(out, maxIovecs);
        });
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
        detail::visitRingBufferImpl(m_impl, [length](auto& impl) { impl.produce(length); });
    }

    /**
     * @brief 消费头部字节并推进读指针
     * @param length 要消费的字节数；超过 readable() 时自动截断
     */
    void consume(size_t length) noexcept {
        detail::visitRingBufferImpl(m_impl, [length](auto& impl) { impl.consume(length); });
    }

    /**
     * @brief 清空缓冲区但保留容量和当前后端
     */
    void clear() noexcept {
        detail::visitRingBufferImpl(m_impl, [](auto& impl) { impl.clear(); });
    }

    /**
     * @brief 写入原始字节
     * @param data 源数据指针；length 为 0 时可以为 nullptr
     * @param length 请求写入字节数
     * @return 实际写入字节数
     */
    size_t write(const void* data, size_t length) {
        return detail::visitRingBufferImpl(m_impl, [data, length](auto& impl) {
            return impl.write(data, length);
        });
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
        return detail::visitRingBufferImpl(m_impl, [data, length](auto& impl) {
            return impl.read(data, length);
        });
    }

private:
#if GALAY_UTILS_RING_BUFFER_HAS_MMAP
    using RuntimeImpl = std::variant<detail::VectorRingBufferImpl, detail::MmapRingBufferImpl>;
    using Impl = std::conditional_t<Strategy == RingBufferBackendStrategy::Vector,
                                    detail::VectorRingBufferImpl,
                                    RuntimeImpl>;
#else
    using Impl = detail::VectorRingBufferImpl;
#endif

    static Impl makeImpl(size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
#if GALAY_UTILS_RING_BUFFER_HAS_MMAP
        if constexpr (Strategy == RingBufferBackendStrategy::Vector) {
            return Impl(capacity);
        } else {
            const bool use_mmap = Strategy == RingBufferBackendStrategy::Mmap || capacity >= kMmapThreshold;
            if (use_mmap) {
                auto mmap_impl = detail::MmapRingBufferImpl::create(capacity);
                if (mmap_impl) {
                    return Impl{std::in_place_type<detail::MmapRingBufferImpl>, std::move(*mmap_impl)};
                }
            }
            return Impl{std::in_place_type<detail::VectorRingBufferImpl>, capacity};
        }
#else
        return Impl(capacity);
#endif
    }

    Impl m_impl;
};

RingBuffer() -> RingBuffer<>;
RingBuffer(size_t) -> RingBuffer<>;

} // namespace galay::utils

#undef GALAY_UTILS_RING_BUFFER_HAS_IOVEC
#undef GALAY_UTILS_RING_BUFFER_HAS_MMAP

#endif // GALAY_UTILS_CACHE_RING_BUFFER_HPP
