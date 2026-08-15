#include <galay/c/galay-utils-c/utils.h>

#include <openssl/evp.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct galay_utils_bytes_t {
    uint8_t* data;
    size_t size;
};

struct galay_utils_ring_buffer_t {
    uint8_t* data;
    size_t capacity;
    size_t read_pos;
    size_t size;
};

const char* galay_utils_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_utils_bytes_create(const void* data, size_t len,
                                        galay_utils_bytes_t** out)
{
    if (out == NULL || *out != NULL || (data == NULL && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_utils_bytes_t* const bytes = calloc(1, sizeof(*bytes));
    if (bytes == NULL) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (len != 0) {
        bytes->data = malloc(len);
        if (bytes->data == NULL) {
            free(bytes);
            return GALAY_OUT_OF_MEMORY;
        }
        memcpy(bytes->data, data, len);
        bytes->size = len;
    }
    *out = bytes;
    return GALAY_OK;
}

void galay_utils_bytes_destroy(galay_utils_bytes_t** bytes)
{
    if (bytes == NULL || *bytes == NULL) {
        return;
    }
    free((*bytes)->data);
    free(*bytes);
    *bytes = NULL;
}

const void* galay_utils_bytes_data(const galay_utils_bytes_t* bytes)
{
    return bytes == NULL ? NULL : bytes->data;
}

size_t galay_utils_bytes_size(const galay_utils_bytes_t* bytes)
{
    return bytes == NULL ? 0 : bytes->size;
}

size_t galay_utils_bytes_capacity(const galay_utils_bytes_t* bytes)
{
    return galay_utils_bytes_size(bytes);
}

galay_status_t galay_utils_ring_buffer_create(size_t capacity,
                                              galay_utils_ring_buffer_t** out)
{
    if (out == NULL || *out != NULL || capacity == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_utils_ring_buffer_t* const ring = calloc(1, sizeof(*ring));
    if (ring == NULL) {
        return GALAY_OUT_OF_MEMORY;
    }
    ring->data = malloc(capacity);
    if (ring->data == NULL) {
        free(ring);
        return GALAY_OUT_OF_MEMORY;
    }
    ring->capacity = capacity;
    *out = ring;
    return GALAY_OK;
}

void galay_utils_ring_buffer_destroy(galay_utils_ring_buffer_t** ring)
{
    if (ring == NULL || *ring == NULL) {
        return;
    }
    free((*ring)->data);
    free(*ring);
    *ring = NULL;
}

size_t galay_utils_ring_buffer_capacity(const galay_utils_ring_buffer_t* ring)
{
    return ring == NULL ? 0 : ring->capacity;
}

size_t galay_utils_ring_buffer_readable(const galay_utils_ring_buffer_t* ring)
{
    return ring == NULL ? 0 : ring->size;
}

size_t galay_utils_ring_buffer_writable(const galay_utils_ring_buffer_t* ring)
{
    return ring == NULL ? 0 : ring->capacity - ring->size;
}

galay_status_t galay_utils_ring_buffer_write(galay_utils_ring_buffer_t* ring,
                                             const void* data, size_t len,
                                             size_t* actual)
{
    if (actual != NULL) {
        *actual = 0;
    }
    if (ring == NULL || actual == NULL || (data == NULL && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (len > ring->capacity - ring->size) {
        return GALAY_OUT_OF_MEMORY;
    }
    const uint8_t* const bytes = data;
    for (size_t index = 0; index < len; ++index) {
        ring->data[(ring->read_pos + ring->size + index) % ring->capacity] = bytes[index];
    }
    ring->size += len;
    *actual = len;
    return GALAY_OK;
}

galay_status_t galay_utils_ring_buffer_read(galay_utils_ring_buffer_t* ring,
                                            void* out, size_t len,
                                            size_t* actual)
{
    if (actual != NULL) {
        *actual = 0;
    }
    if (ring == NULL || actual == NULL || (out == NULL && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (len > ring->size) {
        return GALAY_OUT_OF_MEMORY;
    }
    uint8_t* const bytes = out;
    for (size_t index = 0; index < len; ++index) {
        bytes[index] = ring->data[(ring->read_pos + index) % ring->capacity];
    }
    ring->read_pos = (ring->read_pos + len) % ring->capacity;
    ring->size -= len;
    *actual = len;
    return GALAY_OK;
}

galay_status_t galay_utils_base64_encode(const void* data, size_t len, char* out,
                                         size_t out_len, size_t* actual)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (actual == NULL || out == NULL || (data == NULL && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t groups = len / 3 + (len % 3 != 0);
    if (groups > SIZE_MAX / 4) {
        return GALAY_OUT_OF_MEMORY;
    }
    const size_t required = groups * 4;
    *actual = required;
    if (out_len < required) {
        return GALAY_OUT_OF_MEMORY;
    }
    const uint8_t* const bytes = data;
    size_t pos = 0;
    for (size_t index = 0; index < len; index += 3) {
        const uint32_t b0 = bytes[index];
        const uint32_t b1 = index + 1 < len ? bytes[index + 1] : 0;
        const uint32_t b2 = index + 2 < len ? bytes[index + 2] : 0;
        const uint32_t value = (b0 << 16) | (b1 << 8) | b2;
        out[pos++] = table[(value >> 18) & 0x3f];
        out[pos++] = table[(value >> 12) & 0x3f];
        out[pos++] = index + 1 < len ? table[(value >> 6) & 0x3f] : '=';
        out[pos++] = index + 2 < len ? table[value & 0x3f] : '=';
    }
    return GALAY_OK;
}

static int base64_value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

galay_status_t galay_utils_base64_decode(const char* data, size_t len, void* out,
                                         size_t out_len, size_t* actual)
{
    if (actual == NULL || data == NULL || out == NULL || len % 4 != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    size_t required = (len / 4) * 3;
    if (len != 0 && data[len - 1] == '=') --required;
    if (len > 1 && data[len - 2] == '=') --required;
    *actual = required;
    if (out_len < required) {
        return GALAY_OUT_OF_MEMORY;
    }
    uint8_t* const bytes = out;
    size_t pos = 0;
    for (size_t index = 0; index < len; index += 4) {
        int values[4] = {0, 0, 0, 0};
        const int last_group = index + 4 == len;
        for (size_t offset = 0; offset < 4; ++offset) {
            const unsigned char ch = (unsigned char)data[index + offset];
            if (ch == '=') {
                if (!last_group || offset < 2 ||
                    (offset == 2 && data[index + 3] != '=')) {
                    return GALAY_INVALID_ARGUMENT;
                }
            } else {
                values[offset] = base64_value(ch);
                if (values[offset] < 0) {
                    return GALAY_INVALID_ARGUMENT;
                }
            }
        }
        if (last_group && data[index + 2] == '=' && (values[1] & 0x0f) != 0) {
            return GALAY_INVALID_ARGUMENT;
        }
        if (last_group && data[index + 3] == '=' && data[index + 2] != '=' &&
            (values[2] & 0x03) != 0) {
            return GALAY_INVALID_ARGUMENT;
        }
        const uint32_t value = ((uint32_t)values[0] << 18) |
                               ((uint32_t)values[1] << 12) |
                               ((uint32_t)values[2] << 6) |
                               (uint32_t)values[3];
        if (pos < required) bytes[pos++] = (uint8_t)(value >> 16);
        if (pos < required) bytes[pos++] = (uint8_t)(value >> 8);
        if (pos < required) bytes[pos++] = (uint8_t)value;
    }
    return GALAY_OK;
}

static galay_status_t digest(const EVP_MD* algorithm, const void* data, size_t len,
                             void* out, size_t out_len, size_t required)
{
    if (out == NULL || (data == NULL && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (out_len < required) {
        return GALAY_OUT_OF_MEMORY;
    }
    EVP_MD_CTX* const context = EVP_MD_CTX_new();
    if (context == NULL) {
        return GALAY_OUT_OF_MEMORY;
    }
    unsigned int digest_len = 0;
    const int ok = EVP_DigestInit_ex(context, algorithm, NULL) == 1 &&
                   EVP_DigestUpdate(context, data == NULL ? "" : data, len) == 1 &&
                   EVP_DigestFinal_ex(context, out, &digest_len) == 1;
    EVP_MD_CTX_free(context);
    return ok && digest_len == required ? GALAY_OK : GALAY_INTERNAL_ERROR;
}

galay_status_t galay_utils_md5(const void* data, size_t len, void* out,
                               size_t out_len)
{
    return digest(EVP_md5(), data, len, out, out_len, 16);
}

galay_status_t galay_utils_sha1(const void* data, size_t len, void* out,
                                size_t out_len)
{
    return digest(EVP_sha1(), data, len, out, out_len, 20);
}

static uint32_t rotate_left(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

galay_status_t galay_utils_murmur3_32(const void* data, size_t len,
                                     uint32_t seed, uint32_t* out)
{
    if (out == NULL || (data == NULL && len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const uint8_t* const bytes = data;
    uint32_t hash = seed;
    const size_t block_count = len / 4;
    for (size_t index = 0; index < block_count; ++index) {
        const size_t offset = index * 4;
        uint32_t value = (uint32_t)bytes[offset] |
                         ((uint32_t)bytes[offset + 1] << 8) |
                         ((uint32_t)bytes[offset + 2] << 16) |
                         ((uint32_t)bytes[offset + 3] << 24);
        value *= 0xcc9e2d51U;
        value = rotate_left(value, 15);
        value *= 0x1b873593U;
        hash ^= value;
        hash = rotate_left(hash, 13) * 5U + 0xe6546b64U;
    }
    uint32_t tail = 0;
    const uint8_t* const remaining = bytes == NULL ? NULL : bytes + block_count * 4;
    switch (len & 3U) {
    case 3:
        tail ^= (uint32_t)remaining[2] << 16;
        /* fall through */
    case 2:
        tail ^= (uint32_t)remaining[1] << 8;
        /* fall through */
    case 1:
        tail ^= remaining[0];
        tail *= 0xcc9e2d51U;
        tail = rotate_left(tail, 15);
        tail *= 0x1b873593U;
        hash ^= tail;
        break;
    default:
        break;
    }
    hash ^= (uint32_t)len;
    hash ^= hash >> 16;
    hash *= 0x85ebca6bU;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35U;
    hash ^= hash >> 16;
    *out = hash;
    return GALAY_OK;
}
