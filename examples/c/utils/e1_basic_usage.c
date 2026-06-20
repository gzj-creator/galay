#include <galay/c/galay-utils/utils.h>

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    char encoded[8];
    size_t encoded_len = 0;

    if (galay_utils_base64_encode(payload, sizeof(payload), encoded, sizeof(encoded), &encoded_len) != GALAY_UTILS_OK) {
        return 1;
    }

    printf("base64 length: %zu\n", encoded_len);
    return 0;
}
