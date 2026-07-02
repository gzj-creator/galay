#include <galay/c/galay-utils-c/utils_c.h>

#include <galay/cpp/galay-utils/algorithm/consistent_hash.hpp>
#include <galay/cpp/galay-utils/crypto/md5.hpp>
#include <galay/cpp/galay-utils/crypto/sha1.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

struct galay_utils_bytes_t {
    std::vector<uint8_t> data;
};

struct galay_utils_ring_buffer_t {
    std::vector<uint8_t> data;
    size_t read_pos = 0;
    size_t size = 0;
};

extern "C" {

const char* galay_utils_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_utils_bytes_create(const void* data, size_t len,
                                        galay_utils_bytes_t** out)
{
    if (out == nullptr || (data == nullptr && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* bytes = new (std::nothrow) galay_utils_bytes_t();
    if (bytes == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    if (len != 0) {
        const auto* first = static_cast<const uint8_t*>(data);
        bytes->data.assign(first, first + len);
    }
    *out = bytes;
    return GALAY_OK;
}

void galay_utils_bytes_destroy(galay_utils_bytes_t** bytes)
{
    if (bytes == nullptr || *bytes == nullptr) {
        return;
    }
    delete *bytes;
    *bytes = nullptr;
}

const void* galay_utils_bytes_data(const galay_utils_bytes_t* bytes)
{
    return bytes == nullptr || bytes->data.empty() ? nullptr : bytes->data.data();
}

size_t galay_utils_bytes_size(const galay_utils_bytes_t* bytes)
{
    return bytes == nullptr ? 0 : bytes->data.size();
}

size_t galay_utils_bytes_capacity(const galay_utils_bytes_t* bytes)
{
    return bytes == nullptr ? 0 : bytes->data.capacity();
}

galay_status_t galay_utils_ring_buffer_create(size_t capacity,
                                              galay_utils_ring_buffer_t** out)
{
    if (out == nullptr || capacity == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* ring = new (std::nothrow) galay_utils_ring_buffer_t();
    if (ring == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    ring->data.resize(capacity);
    *out = ring;
    return GALAY_OK;
}

void galay_utils_ring_buffer_destroy(galay_utils_ring_buffer_t** ring)
{
    if (ring == nullptr || *ring == nullptr) {
        return;
    }
    delete *ring;
    *ring = nullptr;
}

size_t galay_utils_ring_buffer_capacity(const galay_utils_ring_buffer_t* ring)
{
    return ring == nullptr ? 0 : ring->data.size();
}

size_t galay_utils_ring_buffer_readable(const galay_utils_ring_buffer_t* ring)
{
    return ring == nullptr ? 0 : ring->size;
}

size_t galay_utils_ring_buffer_writable(const galay_utils_ring_buffer_t* ring)
{
    return ring == nullptr ? 0 : ring->data.size() - ring->size;
}

galay_status_t galay_utils_ring_buffer_write(galay_utils_ring_buffer_t* ring,
                                             const void* data, size_t len,
                                             size_t* actual)
{
    if (actual != nullptr) {
        *actual = 0;
    }
    if (ring == nullptr || (data == nullptr && len != 0) || actual == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (len > ring->data.size() - ring->size) {
        return GALAY_OUT_OF_MEMORY;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        const size_t pos = (ring->read_pos + ring->size + i) % ring->data.size();
        ring->data[pos] = bytes[i];
    }
    ring->size += len;
    *actual = len;
    return GALAY_OK;
}

galay_status_t galay_utils_ring_buffer_read(galay_utils_ring_buffer_t* ring,
                                            void* out, size_t len,
                                            size_t* actual)
{
    if (actual != nullptr) {
        *actual = 0;
    }
    if (ring == nullptr || (out == nullptr && len != 0) || actual == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (len > ring->size) {
        return GALAY_OUT_OF_MEMORY;
    }
    auto* bytes = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < len; ++i) {
        bytes[i] = ring->data[(ring->read_pos + i) % ring->data.size()];
    }
    ring->read_pos = (ring->read_pos + len) % ring->data.size();
    ring->size -= len;
    *actual = len;
    return GALAY_OK;
}

galay_status_t galay_utils_base64_encode(const void* data, size_t len, char* out,
                                         size_t out_len, size_t* actual)
{
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (actual == nullptr || (data == nullptr && len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t required = ((len + 2) / 3) * 4;
    *actual = required;
    if (out_len < required) {
        return GALAY_OUT_OF_MEMORY;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t pos = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = i + 1 < len ? bytes[i + 1] : 0;
        const uint32_t b2 = i + 2 < len ? bytes[i + 2] : 0;
        const uint32_t n = (b0 << 16U) | (b1 << 8U) | b2;
        out[pos++] = table[(n >> 18U) & 0x3FU];
        out[pos++] = table[(n >> 12U) & 0x3FU];
        out[pos++] = i + 1 < len ? table[(n >> 6U) & 0x3FU] : '=';
        out[pos++] = i + 2 < len ? table[n & 0x3FU] : '=';
    }
    return GALAY_OK;
}

galay_status_t galay_utils_base64_decode(const char* data, size_t len, void* out,
                                         size_t out_len, size_t* actual)
{
    if (actual == nullptr || data == nullptr || out == nullptr || len % 4 != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::array<int, 256> map{};
    map.fill(-1);
    const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) {
        map[static_cast<unsigned char>(table[i])] = i;
    }
    size_t required = (len / 4) * 3;
    if (len != 0 && data[len - 1] == '=') {
        --required;
    }
    if (len > 1 && data[len - 2] == '=') {
        --required;
    }
    *actual = required;
    if (out_len < required) {
        return GALAY_OUT_OF_MEMORY;
    }
    auto* bytes = static_cast<uint8_t*>(out);
    size_t pos = 0;
    for (size_t i = 0; i < len; i += 4) {
        int vals[4] = {0, 0, 0, 0};
        for (size_t j = 0; j < 4; ++j) {
            const unsigned char ch = static_cast<unsigned char>(data[i + j]);
            if (ch == '=') {
                vals[j] = 0;
            } else if (map[ch] >= 0) {
                vals[j] = map[ch];
            } else {
                return GALAY_INVALID_ARGUMENT;
            }
        }
        const uint32_t n = (static_cast<uint32_t>(vals[0]) << 18U) |
            (static_cast<uint32_t>(vals[1]) << 12U) |
            (static_cast<uint32_t>(vals[2]) << 6U) |
            static_cast<uint32_t>(vals[3]);
        if (pos < required) {
            bytes[pos++] = static_cast<uint8_t>((n >> 16U) & 0xFFU);
        }
        if (pos < required) {
            bytes[pos++] = static_cast<uint8_t>((n >> 8U) & 0xFFU);
        }
        if (pos < required) {
            bytes[pos++] = static_cast<uint8_t>(n & 0xFFU);
        }
    }
    return GALAY_OK;
}

galay_status_t galay_utils_md5(const void* data, size_t len, void* out,
                               size_t out_len)
{
    if ((data == nullptr && len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (out_len < 16) {
        return GALAY_OUT_OF_MEMORY;
    }
    const auto digest = galay::utils::MD5Util::MD5Raw(static_cast<const unsigned char*>(data), len);
    std::memcpy(out, digest.data(), digest.size());
    return GALAY_OK;
}

galay_status_t galay_utils_sha1(const void* data, size_t len, void* out,
                                size_t out_len)
{
    if ((data == nullptr && len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (out_len < 20) {
        return GALAY_OUT_OF_MEMORY;
    }
    const auto digest = galay::utils::SHA1::hash(static_cast<const uint8_t*>(data), len);
    std::memcpy(out, digest.data(), digest.size());
    return GALAY_OK;
}

galay_status_t galay_utils_murmur3_32(const void* data, size_t len,
                                      uint32_t seed, uint32_t* out)
{
    if ((data == nullptr && len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = galay::utils::MurmurHash3::hash32(data, len, seed);
    return GALAY_OK;
}

}
