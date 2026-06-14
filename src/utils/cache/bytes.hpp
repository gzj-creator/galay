/**
 * @file bytes.hpp
 * @brief Move-only byte container with optional ownership
 * @author galay-utils
 * @version 1.0.0
 *
 * @details Provides a raw byte metadata descriptor and a small move-only
 *          container for owned byte buffers or non-owning views. The API is
 *          not thread-safe; callers must synchronize concurrent access.
 */

#ifndef GALAY_UTILS_CACHE_BYTES_HPP
#define GALAY_UTILS_CACHE_BYTES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

namespace galay::utils {

/**
 * @brief Raw byte buffer metadata
 * @details Describes a byte pointer, readable size, and total capacity. This
 *          type does not own memory by itself; ownership belongs to the caller
 *          or to a container such as `Bytes`.
 */
struct ByteMetaData {
    /**
     * @brief Construct empty metadata
     */
    ByteMetaData() = default;

    /**
     * @brief Construct a non-owning view over a mutable string
     * @param str Source string that must outlive this metadata view
     */
    explicit ByteMetaData(std::string& str) noexcept
        : data(reinterpret_cast<uint8_t*>(str.data()))
        , size(str.size())
        , capacity(str.capacity()) {}

    /**
     * @brief Construct a non-owning view over a string view
     * @param str Source view whose backing storage must outlive this metadata
     */
    explicit ByteMetaData(std::string_view str) noexcept
        : data(reinterpret_cast<uint8_t*>(const_cast<char*>(str.data())))
        , size(str.size())
        , capacity(str.size()) {}

    /**
     * @brief Construct a non-owning view over a null-terminated C string
     * @param str Source pointer; nullptr creates an empty metadata value
     */
    explicit ByteMetaData(const char* str) noexcept
        : data(reinterpret_cast<uint8_t*>(const_cast<char*>(str))) {
        size = str == nullptr ? 0 : std::strlen(str);
        capacity = size;
    }

    /**
     * @brief Construct a non-owning view over a null-terminated byte string
     * @param str Source pointer; nullptr creates an empty metadata value
     */
    explicit ByteMetaData(const uint8_t* str) noexcept
        : ByteMetaData(reinterpret_cast<const char*>(str)) {}

    /**
     * @brief Construct a non-owning view over an explicit byte range
     * @param str Source pointer; nullptr creates an empty metadata value
     * @param length Readable byte count and capacity
     */
    ByteMetaData(const char* str, size_t length) noexcept
        : data(reinterpret_cast<uint8_t*>(const_cast<char*>(str)))
        , size(str == nullptr ? 0 : length)
        , capacity(str == nullptr ? 0 : length) {}

    /**
     * @brief Construct a non-owning view over an explicit byte range
     * @param str Source pointer; nullptr creates an empty metadata value
     * @param length Readable byte count and capacity
     */
    ByteMetaData(const uint8_t* str, size_t length) noexcept
        : ByteMetaData(reinterpret_cast<const char*>(str), length) {}

    uint8_t* data = nullptr; ///< Pointer to the first byte
    size_t size = 0;         ///< Number of readable bytes
    size_t capacity = 0;     ///< Total available capacity in bytes
};

/**
 * @brief Allocate raw byte metadata with the requested capacity
 * @param length Capacity to allocate
 * @return Metadata with size 0 and capacity `length`; empty when length is 0
 * @throws std::bad_alloc when allocation fails
 */
inline ByteMetaData mallocBytes(size_t length) {
    ByteMetaData meta;
    if (length == 0) {
        return meta;
    }

    meta.data = static_cast<uint8_t*>(std::malloc(length));
    if (meta.data == nullptr) {
        throw std::bad_alloc();
    }
    meta.size = 0;
    meta.capacity = length;
    return meta;
}

/**
 * @brief Deep-copy byte metadata into newly allocated storage
 * @param meta Source metadata; only `meta.size` readable bytes are copied
 * @return New metadata owning an independent allocation
 * @throws std::bad_alloc when allocation fails
 */
inline ByteMetaData deepCopyBytes(const ByteMetaData& meta) {
    ByteMetaData copy = mallocBytes(meta.capacity);
    copy.size = std::min(meta.size, copy.capacity);
    if (copy.data != nullptr && meta.data != nullptr && copy.size > 0) {
        std::memcpy(copy.data, meta.data, copy.size);
    }
    return copy;
}

/**
 * @brief Reallocate raw byte metadata to a new capacity
 * @param meta Metadata to resize
 * @param length New capacity; 0 frees and resets the metadata
 * @throws std::bad_alloc when allocation fails
 */
inline void reallocBytes(ByteMetaData& meta, size_t length) {
    if (length == 0) {
        if (meta.data != nullptr) {
            std::free(meta.data);
        }
        meta.data = nullptr;
        meta.size = 0;
        meta.capacity = 0;
        return;
    }

    void* resized = std::realloc(meta.data, length);
    if (resized == nullptr) {
        throw std::bad_alloc();
    }

    meta.data = static_cast<uint8_t*>(resized);
    meta.capacity = length;
    if (meta.size > length) {
        meta.size = length;
    }
}

/**
 * @brief Clear bytes without freeing allocated storage
 * @param meta Metadata whose storage should be zeroed and marked empty
 */
inline void clearBytes(ByteMetaData& meta) noexcept {
    if (meta.data != nullptr && meta.capacity > 0) {
        std::memset(meta.data, 0, meta.capacity);
    }
    meta.size = 0;
}

/**
 * @brief Free raw byte metadata storage and reset all fields
 * @param meta Metadata to release
 */
inline void freeBytes(ByteMetaData& meta) noexcept {
    if (meta.data != nullptr) {
        std::free(meta.data);
    }
    meta.data = nullptr;
    meta.size = 0;
    meta.capacity = 0;
}

/**
 * @brief Move-only byte sequence container with optional ownership
 * @details Owning constructors deep-copy source bytes. `fromString()` and
 *          `fromCString()` create non-owning views, so the backing storage must
 *          outlive the returned `Bytes` object.
 */
class Bytes {
public:
    /**
     * @brief Construct an empty byte container
     */
    Bytes() = default;

    /**
     * @brief Deep-copy a string into owned storage
     * @param str Source string
     */
    explicit Bytes(std::string& str)
        : Bytes(str.data(), str.size()) {}

    /**
     * @brief Deep-copy a string into owned storage
     * @param str Source string
     */
    explicit Bytes(std::string&& str)
        : Bytes(str.data(), str.size()) {}

    /**
     * @brief Deep-copy a null-terminated C string into owned storage
     * @param str Source pointer; nullptr creates an empty container
     */
    explicit Bytes(const char* str)
        : Bytes(str, str == nullptr ? 0 : std::strlen(str)) {}

    /**
     * @brief Deep-copy a null-terminated byte string into owned storage
     * @param str Source pointer; nullptr creates an empty container
     */
    explicit Bytes(const uint8_t* str)
        : Bytes(reinterpret_cast<const char*>(str)) {}

    /**
     * @brief Deep-copy an explicit char range into owned storage
     * @param str Source pointer
     * @param length Number of bytes to copy
     */
    Bytes(const char* str, size_t length) {
        assignOwned(reinterpret_cast<const uint8_t*>(str), length);
    }

    /**
     * @brief Deep-copy an explicit byte range into owned storage
     * @param str Source pointer
     * @param length Number of bytes to copy
     */
    Bytes(const uint8_t* str, size_t length) {
        assignOwned(str, length);
    }

    /**
     * @brief Allocate owned storage with size 0
     * @param capacity Number of bytes to allocate
     */
    explicit Bytes(size_t capacity)
        : m_meta(mallocBytes(capacity))
        , m_owned(capacity > 0) {}

    /**
     * @brief Move-construct by transferring ownership and metadata
     * @param other Source object; left empty
     */
    Bytes(Bytes&& other) noexcept
        : m_meta(other.m_meta)
        , m_owned(other.m_owned) {
        other.reset();
    }

    Bytes(const Bytes&) = delete;

    /**
     * @brief Move-assign by releasing current owned storage and taking other
     * @param other Source object; left empty
     * @return This object
     */
    Bytes& operator=(Bytes&& other) noexcept {
        if (this != &other) {
            clear();
            m_meta = other.m_meta;
            m_owned = other.m_owned;
            other.reset();
        }
        return *this;
    }

    Bytes& operator=(const Bytes&) = delete;

    /**
     * @brief Release owned storage if present
     */
    ~Bytes() {
        clear();
    }

    /**
     * @brief Create a non-owning view over a mutable string
     * @param str Source string that must outlive the returned object
     * @return Non-owning byte container
     */
    static Bytes fromString(std::string& str) noexcept {
        Bytes bytes;
        bytes.m_meta = ByteMetaData(str);
        bytes.m_owned = false;
        return bytes;
    }

    /**
     * @brief Create a non-owning view over a string view
     * @param str Source view whose backing storage must outlive the returned object
     * @return Non-owning byte container
     */
    static Bytes fromString(std::string_view str) noexcept {
        Bytes bytes;
        bytes.m_meta = ByteMetaData(str);
        bytes.m_owned = false;
        return bytes;
    }

    /**
     * @brief Create a non-owning view over an explicit char range
     * @param str Source pointer that must outlive the returned object
     * @param length Readable byte count
     * @param capacity Total available capacity
     * @return Non-owning byte container
     */
    static Bytes fromCString(const char* str, size_t length, size_t capacity) noexcept {
        Bytes bytes;
        bytes.m_meta.data = reinterpret_cast<uint8_t*>(const_cast<char*>(str));
        bytes.m_meta.size = str == nullptr ? 0 : length;
        bytes.m_meta.capacity = str == nullptr ? 0 : capacity;
        bytes.m_owned = false;
        return bytes;
    }

    /**
     * @brief Get the byte data pointer
     * @return Pointer to readable bytes, or nullptr when empty
     */
    [[nodiscard]] const uint8_t* data() const noexcept {
        return m_meta.data;
    }

    /**
     * @brief Get data as a C string when storage can safely expose one
     * @return Pointer to character data, or nullptr when no data exists
     * @note Writes a null terminator only when capacity is greater than size.
     */
    [[nodiscard]] const char* c_str() const noexcept {
        if (m_meta.data == nullptr) {
            return nullptr;
        }
        if (m_meta.size > 0 && m_meta.data[m_meta.size - 1] != '\0' &&
            m_meta.size < m_meta.capacity) {
            m_meta.data[m_meta.size] = '\0';
        }
        return reinterpret_cast<const char*>(m_meta.data);
    }

    /**
     * @brief Get readable byte count
     * @return Number of bytes
     */
    [[nodiscard]] size_t size() const noexcept {
        return m_meta.size;
    }

    /**
     * @brief Get storage capacity
     * @return Capacity in bytes
     */
    [[nodiscard]] size_t capacity() const noexcept {
        return m_meta.capacity;
    }

    /**
     * @brief Check whether there are no readable bytes
     * @return true when size is zero
     */
    [[nodiscard]] bool empty() const noexcept {
        return m_meta.size == 0;
    }

    /**
     * @brief Release owned storage or detach from a non-owning view
     */
    void clear() noexcept {
        if (m_owned) {
            freeBytes(m_meta);
        } else {
            reset();
        }
    }

    /**
     * @brief Copy readable bytes into a string
     * @return String containing current bytes
     */
    [[nodiscard]] std::string toString() const {
        if (m_meta.data == nullptr || m_meta.size == 0) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(m_meta.data), m_meta.size);
    }

    /**
     * @brief View readable bytes as a string view
     * @return Non-owning view into current bytes
     */
    [[nodiscard]] std::string_view toStringView() const noexcept {
        if (m_meta.data == nullptr || m_meta.size == 0) {
            return {};
        }
        return std::string_view(reinterpret_cast<const char*>(m_meta.data), m_meta.size);
    }

    /**
     * @brief Compare byte contents
     * @param other Other byte container
     * @return true when sizes and bytes match
     */
    [[nodiscard]] bool operator==(const Bytes& other) const noexcept {
        if (m_meta.size != other.m_meta.size) {
            return false;
        }
        if (m_meta.size == 0) {
            return true;
        }
        if (m_meta.data == other.m_meta.data) {
            return true;
        }
        if (m_meta.data == nullptr || other.m_meta.data == nullptr) {
            return false;
        }
        return std::memcmp(m_meta.data, other.m_meta.data, m_meta.size) == 0;
    }

    /**
     * @brief Compare byte contents for inequality
     * @param other Other byte container
     * @return true when size or bytes differ
     */
    [[nodiscard]] bool operator!=(const Bytes& other) const noexcept {
        return !(*this == other);
    }

private:
    void assignOwned(const uint8_t* data, size_t length) {
        if (data == nullptr || length == 0) {
            reset();
            return;
        }

        m_meta = mallocBytes(length);
        std::memcpy(m_meta.data, data, length);
        m_meta.size = length;
        m_owned = true;
    }

    void reset() noexcept {
        m_meta.data = nullptr;
        m_meta.size = 0;
        m_meta.capacity = 0;
        m_owned = false;
    }

    ByteMetaData m_meta;  ///< Data pointer, readable size, and capacity
    bool m_owned = false; ///< True when this object must free the data
};

} // namespace galay::utils

#endif // GALAY_UTILS_CACHE_BYTES_HPP
