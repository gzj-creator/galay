#include <galay/c/galay-utils/utils.h>

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    static const uint8_t payload[] = "galay-c-utils";
    char encoded[32];
    uint8_t digest[16];
    size_t encoded_len = 0;
    uint32_t hash = 0;

    for (size_t i = 0; i < 1000; ++i) {
        if (galay_utils_base64_encode(payload, sizeof(payload) - 1, encoded, sizeof(encoded), &encoded_len) != GALAY_UTILS_OK) {
            return 1;
        }
        if (galay_utils_md5(payload, sizeof(payload) - 1, digest, sizeof(digest)) != GALAY_UTILS_OK) {
            return 1;
        }
        if (galay_utils_murmur3_32(payload, sizeof(payload) - 1, (uint32_t)i, &hash) != GALAY_UTILS_OK) {
            return 1;
        }
    }

    printf("codec/hash smoke: encoded=%zu hash=%u\n", encoded_len, hash);
    return 0;
}
