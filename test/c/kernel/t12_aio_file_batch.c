#include <galay/c/galay-kernel-c/async-c/aio_file.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int expect_code(C_IOResult actual, C_IOResultCode expected)
{
    return actual.code == expected ? 0 : 1;
}

int main(void)
{
    galay_c_aio_file_t invalid = {.fd = -1};
    galay_c_aio_backend_t backend = GALAY_C_AIO_BACKEND_FALLBACK;
    char byte = 0;
    if (expect_code(galay_c_aio_check_support(NULL), C_IOResultInvalid) ||
        expect_code(galay_c_aio_file_open(NULL, "/tmp/missing", GALAY_C_AIO_READ, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_aio_file_read(NULL, &byte, 1, 0, 0), C_IOResultInvalid) ||
        expect_code(galay_c_aio_file_write(&invalid, NULL, 1, 0, 0),
                    C_IOResultInvalid) ||
        expect_code(galay_c_aio_file_close(NULL), C_IOResultInvalid) ||
        expect_code(galay_c_aio_file_close(&invalid), C_IOResultOk)) {
        return 1;
    }

    const C_IOResult support = galay_c_aio_check_support(&backend);
    if (support.code != C_IOResultOk || backend != GALAY_C_AIO_BACKEND_IOURING) {
        return 0;
    }

    char path[] = "/tmp/galay-c-aio-XXXXXX";
    const int temp_fd = mkstemp(path);
    if (temp_fd < 0) {
        return 2;
    }
    if (close(temp_fd) != 0) {
        return 3;
    }

    enum { block_size = 4096 };
    char* const write_buffer = aligned_alloc(block_size, block_size);
    char* const read_buffer = aligned_alloc(block_size, block_size);
    galay_c_aio_file_t file = {.fd = -1};
    int result = 0;
    if (write_buffer == NULL || read_buffer == NULL) {
        result = 4;
        goto cleanup;
    }
    memset(write_buffer, 0x5a, block_size);
    memset(read_buffer, 0, block_size);

    if (galay_c_aio_file_open(
            &file, path,
            GALAY_C_AIO_RDWR | GALAY_C_AIO_CREATE | GALAY_C_AIO_TRUNC,
            0600).code != C_IOResultOk) {
        result = 5;
        goto cleanup;
    }
    const C_IOResult written =
        galay_c_aio_file_write(&file, write_buffer, block_size, 0, 5000);
    const C_IOResult synced = galay_c_aio_file_fsync(&file, 5000);
    const C_IOResult read =
        galay_c_aio_file_read(&file, read_buffer, block_size, 0, 5000);
    if (written.code != C_IOResultOk || written.bytes != block_size ||
        synced.code != C_IOResultOk || read.code != C_IOResultOk ||
        read.bytes != block_size || memcmp(write_buffer, read_buffer, block_size) != 0) {
        result = 6;
    }

cleanup:
    if (file.fd >= 0 && galay_c_aio_file_close(&file).code != C_IOResultOk && result == 0) {
        result = 7;
    }
    free(write_buffer);
    free(read_buffer);
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 8;
    }
    return result;
}
