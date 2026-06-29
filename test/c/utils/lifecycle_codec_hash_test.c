#include <galay/c/galay-utils-c/utils.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_bytes_lifecycle(void)
{
    const uint8_t input[] = {'a', 'b', 'c'};
    galay_utils_bytes_t* bytes = 0;

    assert(galay_utils_bytes_create(input, sizeof(input), &bytes) == GALAY_UTILS_OK);
    assert(bytes != 0);
    assert(galay_utils_bytes_size(bytes) == sizeof(input));
    assert(galay_utils_bytes_capacity(bytes) >= sizeof(input));
    assert(memcmp(galay_utils_bytes_data(bytes), input, sizeof(input)) == 0);

    galay_utils_bytes_destroy(&bytes);
    assert(bytes == 0);
    galay_utils_bytes_destroy(&bytes);
    galay_utils_bytes_destroy(0);

    assert(galay_utils_bytes_create(input, sizeof(input), 0) == GALAY_UTILS_INVALID_ARGUMENT);
    assert(galay_utils_bytes_create(0, 1, &bytes) == GALAY_UTILS_INVALID_ARGUMENT);
}

static void test_ring_buffer_lifecycle(void)
{
    galay_utils_ring_buffer_t* ring = 0;
    const uint8_t input[] = {'w', 'x', 'y', 'z'};
    uint8_t output[4] = {0};
    size_t actual = 0;

    assert(galay_utils_ring_buffer_create(4, &ring) == GALAY_UTILS_OK);
    assert(ring != 0);
    assert(galay_utils_ring_buffer_capacity(ring) == 4);
    assert(galay_utils_ring_buffer_readable(ring) == 0);
    assert(galay_utils_ring_buffer_writable(ring) == 4);

    assert(galay_utils_ring_buffer_write(ring, input, sizeof(input), &actual) == GALAY_UTILS_OK);
    assert(actual == sizeof(input));
    assert(galay_utils_ring_buffer_writable(ring) == 0);

    assert(galay_utils_ring_buffer_write(ring, input, 1, &actual) == GALAY_UTILS_BUFFER_TOO_SMALL);
    assert(actual == 0);

    assert(galay_utils_ring_buffer_read(ring, output, sizeof(output), &actual) == GALAY_UTILS_OK);
    assert(actual == sizeof(output));
    assert(memcmp(output, input, sizeof(output)) == 0);

    galay_utils_ring_buffer_destroy(&ring);
    assert(ring == 0);
    galay_utils_ring_buffer_destroy(&ring);

    assert(galay_utils_ring_buffer_create(0, &ring) == GALAY_UTILS_INVALID_ARGUMENT);
    assert(galay_utils_ring_buffer_write(0, input, sizeof(input), &actual) == GALAY_UTILS_INVALID_ARGUMENT);
}

static void test_base64_codec_boundaries(void)
{
    const uint8_t input[] = {'h', 'e', 'l', 'l', 'o'};
    char encoded[8] = {0};
    uint8_t decoded[5] = {0};
    size_t actual = 0;

    assert(galay_utils_base64_encode(input, sizeof(input), encoded, sizeof(encoded), &actual) == GALAY_UTILS_OK);
    assert(actual == 8);
    assert(memcmp(encoded, "aGVsbG8=", 8) == 0);

    assert(galay_utils_base64_encode(input, sizeof(input), encoded, 4, &actual) == GALAY_UTILS_BUFFER_TOO_SMALL);
    assert(actual == 8);
    assert(galay_utils_base64_encode(0, 1, encoded, sizeof(encoded), &actual) == GALAY_UTILS_INVALID_ARGUMENT);

    assert(galay_utils_base64_decode(encoded, actual, decoded, sizeof(decoded), &actual) == GALAY_UTILS_OK);
    assert(actual == sizeof(decoded));
    assert(memcmp(decoded, input, sizeof(input)) == 0);

    assert(galay_utils_base64_decode(encoded, 8, decoded, 4, &actual) == GALAY_UTILS_BUFFER_TOO_SMALL);
    assert(actual == 5);
    assert(galay_utils_base64_decode("!!!", 3, decoded, sizeof(decoded), &actual) == GALAY_UTILS_INVALID_ARGUMENT);
}

static void test_hash_wrappers(void)
{
    const uint8_t input[] = {'a', 'b', 'c'};
    uint8_t md5[16] = {0};
    uint8_t sha1[20] = {0};
    uint32_t hash32 = 1;
    static const uint8_t expected_md5[16] = {
        0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
        0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72
    };
    static const uint8_t expected_sha1[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };

    assert(galay_utils_md5(input, sizeof(input), md5, sizeof(md5)) == GALAY_UTILS_OK);
    assert(memcmp(md5, expected_md5, sizeof(md5)) == 0);
    assert(galay_utils_sha1(input, sizeof(input), sha1, sizeof(sha1)) == GALAY_UTILS_OK);
    assert(memcmp(sha1, expected_sha1, sizeof(sha1)) == 0);
    assert(galay_utils_murmur3_32(0, 0, 0, &hash32) == GALAY_UTILS_OK);
    assert(hash32 == 0);

    assert(galay_utils_md5(input, sizeof(input), md5, 15) == GALAY_UTILS_BUFFER_TOO_SMALL);
    assert(galay_utils_sha1(0, 1, sha1, sizeof(sha1)) == GALAY_UTILS_INVALID_ARGUMENT);
    assert(galay_utils_murmur3_32(input, sizeof(input), 0, 0) == GALAY_UTILS_INVALID_ARGUMENT);
}

int main(void)
{
    test_bytes_lifecycle();
    test_ring_buffer_lifecycle();
    test_base64_codec_boundaries();
    test_hash_wrappers();
    return 0;
}
