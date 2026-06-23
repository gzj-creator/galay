#include <galay/c/galay-utils/utils.h>

#include <galay/cpp/galay-utils/cache/bytes.hpp>
#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>
#include <galay/cpp/galay-utils/crypto/md5.hpp>
#include <galay/cpp/galay-utils/crypto/murmur_hash3.hpp>
#include <galay/cpp/galay-utils/crypto/sha1.hpp>
#include <galay/cpp/galay-utils/encoding/base64.hpp>

#include <cstring>
#include <new>
#include <string>
#include <string_view>

struct galay_utils_bytes {
    galay::utils::Bytes value;
};

struct galay_utils_ring_buffer {
    galay::utils::RingBuffer value;
};

namespace {

galay_utils_status_t map_exception(const std::exception&) noexcept
{
    return GALAY_UTILS_INTERNAL_ERROR;
}

bool invalid_input_span(const void* data, size_t length) noexcept
{
    return data == nullptr && length != 0;
}

const uint8_t* stable_bytes(const void* data, size_t length) noexcept
{
    static constexpr uint8_t kEmpty = 0;
    return length == 0 ? &kEmpty : static_cast<const uint8_t*>(data);
}

galay_utils_status_t copy_string_result(
    const std::string& result,
    void* output,
    size_t output_capacity,
    size_t* output_length) noexcept
{
    if (output_length == nullptr) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }
    *output_length = result.size();
    if (result.empty()) {
        return GALAY_UTILS_OK;
    }
    if (output == nullptr || output_capacity < result.size()) {
        return GALAY_UTILS_BUFFER_TOO_SMALL;
    }
    std::memcpy(output, result.data(), result.size());
    return GALAY_UTILS_OK;
}

} // namespace

extern "C" {

galay_utils_status_t galay_utils_bytes_create(
    const void* data,
    size_t length,
    galay_utils_bytes_t** out)
{
    if (out == nullptr || invalid_input_span(data, length)) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }

    *out = nullptr;
    try {
        auto* handle = new galay_utils_bytes{
            galay::utils::Bytes(static_cast<const uint8_t*>(data), length)
        };
        *out = handle;
        return GALAY_UTILS_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_UTILS_OUT_OF_MEMORY;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

void galay_utils_bytes_destroy(galay_utils_bytes_t** bytes)
{
    if (bytes == nullptr || *bytes == nullptr) {
        return;
    }
    delete *bytes;
    *bytes = nullptr;
}

const uint8_t* galay_utils_bytes_data(const galay_utils_bytes_t* bytes)
{
    return bytes == nullptr ? nullptr : bytes->value.data();
}

size_t galay_utils_bytes_size(const galay_utils_bytes_t* bytes)
{
    return bytes == nullptr ? 0 : bytes->value.size();
}

size_t galay_utils_bytes_capacity(const galay_utils_bytes_t* bytes)
{
    return bytes == nullptr ? 0 : bytes->value.capacity();
}

galay_utils_status_t galay_utils_ring_buffer_create(
    size_t capacity,
    galay_utils_ring_buffer_t** out)
{
    if (out == nullptr || capacity == 0) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }

    *out = nullptr;
    try {
        auto* handle = new galay_utils_ring_buffer{galay::utils::RingBuffer(capacity)};
        *out = handle;
        return GALAY_UTILS_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_UTILS_OUT_OF_MEMORY;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
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
    return ring == nullptr ? 0 : ring->value.capacity();
}

size_t galay_utils_ring_buffer_readable(const galay_utils_ring_buffer_t* ring)
{
    return ring == nullptr ? 0 : ring->value.readable();
}

size_t galay_utils_ring_buffer_writable(const galay_utils_ring_buffer_t* ring)
{
    return ring == nullptr ? 0 : ring->value.writable();
}

galay_utils_status_t galay_utils_ring_buffer_write(
    galay_utils_ring_buffer_t* ring,
    const void* data,
    size_t length,
    size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    if (ring == nullptr || invalid_input_span(data, length)) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }
    if (length == 0) {
        return GALAY_UTILS_OK;
    }

    try {
        const size_t actual = ring->value.write(data, length);
        if (written != nullptr) {
            *written = actual;
        }
        return actual == length ? GALAY_UTILS_OK : GALAY_UTILS_BUFFER_TOO_SMALL;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

galay_utils_status_t galay_utils_ring_buffer_read(
    galay_utils_ring_buffer_t* ring,
    void* data,
    size_t length,
    size_t* read_bytes)
{
    if (read_bytes != nullptr) {
        *read_bytes = 0;
    }
    if (ring == nullptr || (data == nullptr && length != 0)) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }
    if (length == 0) {
        return GALAY_UTILS_OK;
    }

    try {
        const size_t actual = ring->value.read(data, length);
        if (read_bytes != nullptr) {
            *read_bytes = actual;
        }
        return actual == length ? GALAY_UTILS_OK : GALAY_UTILS_BUFFER_TOO_SMALL;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

void galay_utils_ring_buffer_clear(galay_utils_ring_buffer_t* ring)
{
    if (ring != nullptr) {
        ring->value.clear();
    }
}

galay_utils_status_t galay_utils_base64_encode(
    const void* data,
    size_t length,
    char* output,
    size_t output_capacity,
    size_t* output_length)
{
    if (invalid_input_span(data, length)) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }

    try {
        const auto* bytes = static_cast<const unsigned char*>(data);
        const std::string encoded = galay::utils::Base64Util::Base64Encode(bytes, length);
        return copy_string_result(encoded, output, output_capacity, output_length);
    } catch (const std::bad_alloc&) {
        return GALAY_UTILS_OUT_OF_MEMORY;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

galay_utils_status_t galay_utils_base64_decode(
    const char* data,
    size_t length,
    void* output,
    size_t output_capacity,
    size_t* output_length)
{
    if (data == nullptr && length != 0) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }

    const std::string_view input(data == nullptr ? "" : data, length);
    if (!galay::utils::Base64Util::Base64CanDecodeView(input)) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }
    const std::string decoded = galay::utils::Base64Util::Base64DecodeView(input);
    return copy_string_result(decoded, output, output_capacity, output_length);
}

galay_utils_status_t galay_utils_md5(
    const void* data,
    size_t length,
    uint8_t* output,
    size_t output_capacity)
{
    if (invalid_input_span(data, length) || output == nullptr) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }
    if (output_capacity < 16) {
        return GALAY_UTILS_BUFFER_TOO_SMALL;
    }

    try {
        const auto digest = galay::utils::MD5Util::MD5Raw(stable_bytes(data, length), length);
        std::memcpy(output, digest.data(), digest.size());
        return GALAY_UTILS_OK;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

galay_utils_status_t galay_utils_sha1(
    const void* data,
    size_t length,
    uint8_t* output,
    size_t output_capacity)
{
    if (invalid_input_span(data, length) || output == nullptr) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }
    if (output_capacity < 20) {
        return GALAY_UTILS_BUFFER_TOO_SMALL;
    }

    try {
        const auto digest = galay::utils::SHA1::hash(stable_bytes(data, length), length);
        std::memcpy(output, digest.data(), digest.size());
        return GALAY_UTILS_OK;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

galay_utils_status_t galay_utils_murmur3_32(
    const void* data,
    size_t length,
    uint32_t seed,
    uint32_t* output)
{
    if (invalid_input_span(data, length) || output == nullptr) {
        return GALAY_UTILS_INVALID_ARGUMENT;
    }

    try {
        *output = galay::utils::MurmurHash3Util::Hash32(stable_bytes(data, length), length, seed);
        return GALAY_UTILS_OK;
    } catch (const std::exception& ex) {
        return map_exception(ex);
    } catch (...) {
        return GALAY_UTILS_INTERNAL_ERROR;
    }
}

} // extern "C"
