#include <galay/c/galay-kernel-c/async-c/aio_file.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    AIO_FILE_BATCH_ITERATIONS = 32,
    AIO_FILE_BLOCK_SIZE = 4096,
};

static int64_t now_us(void)
{
    struct timeval value;
    return gettimeofday(&value, NULL) == 0
        ? (int64_t)value.tv_sec * 1000000 + value.tv_usec
        : -1;
}

int main(void)
{
    galay_c_aio_backend_t backend = GALAY_C_AIO_BACKEND_FALLBACK;
    const C_IOResult support = galay_c_aio_check_support(&backend);
    if (support.code != C_IOResultOk || backend != GALAY_C_AIO_BACKEND_IOURING) {
        return printf("aio_file_batch native io_uring backend unavailable\n") < 0 ? 1 : 0;
    }

    char path[] = "/tmp/galay-aio-benchmark-XXXXXX";
    const int temp_fd = mkstemp(path);
    if (temp_fd < 0 || close(temp_fd) != 0) {
        return 2;
    }

    char* const write_a = aligned_alloc(AIO_FILE_BLOCK_SIZE, AIO_FILE_BLOCK_SIZE);
    char* const write_b = aligned_alloc(AIO_FILE_BLOCK_SIZE, AIO_FILE_BLOCK_SIZE);
    char* const read_a = aligned_alloc(AIO_FILE_BLOCK_SIZE, AIO_FILE_BLOCK_SIZE);
    char* const read_b = aligned_alloc(AIO_FILE_BLOCK_SIZE, AIO_FILE_BLOCK_SIZE);
    galay_c_aio_file_t file = {.fd = -1};
    int result = 0;
    if (write_a == NULL || write_b == NULL || read_a == NULL || read_b == NULL) {
        result = 3;
        goto cleanup;
    }
    memset(write_a, 0x41, AIO_FILE_BLOCK_SIZE);
    memset(write_b, 0x6d, AIO_FILE_BLOCK_SIZE);
    if (galay_c_aio_file_open(
            &file, path,
            GALAY_C_AIO_RDWR | GALAY_C_AIO_CREATE | GALAY_C_AIO_TRUNC,
            0600).code != C_IOResultOk) {
        result = 4;
        goto cleanup;
    }

    const int64_t start = now_us();
    for (int iteration = 0; iteration < AIO_FILE_BATCH_ITERATIONS; ++iteration) {
        memset(read_a, 0, AIO_FILE_BLOCK_SIZE);
        memset(read_b, 0, AIO_FILE_BLOCK_SIZE);
        const C_IOResult write_first =
            galay_c_aio_file_write(&file, write_a, AIO_FILE_BLOCK_SIZE, 0, 5000);
        const C_IOResult write_second = galay_c_aio_file_write(
            &file, write_b, AIO_FILE_BLOCK_SIZE, AIO_FILE_BLOCK_SIZE, 5000);
        const C_IOResult read_first =
            galay_c_aio_file_read(&file, read_a, AIO_FILE_BLOCK_SIZE, 0, 5000);
        const C_IOResult read_second = galay_c_aio_file_read(
            &file, read_b, AIO_FILE_BLOCK_SIZE, AIO_FILE_BLOCK_SIZE, 5000);
        if (write_first.code != C_IOResultOk || write_second.code != C_IOResultOk ||
            read_first.code != C_IOResultOk || read_second.code != C_IOResultOk ||
            memcmp(write_a, read_a, AIO_FILE_BLOCK_SIZE) != 0 ||
            memcmp(write_b, read_b, AIO_FILE_BLOCK_SIZE) != 0) {
            result = 5;
            goto cleanup;
        }
    }
    const int64_t elapsed = now_us() - start;
    const double seconds = elapsed > 0 ? (double)elapsed / 1000000.0 : 0.0;
    const size_t operation_count = AIO_FILE_BATCH_ITERATIONS * 4u;
    const size_t byte_count = operation_count * AIO_FILE_BLOCK_SIZE;
    if (printf("aio_file_batch operations=%zu elapsed_ms=%.3f ops_per_sec=%.2f "
               "throughput_mib_per_sec=%.3f\n",
               operation_count,
               (double)elapsed / 1000.0,
               seconds > 0.0 ? operation_count / seconds : 0.0,
               seconds > 0.0 ? byte_count / (1024.0 * 1024.0) / seconds : 0.0) < 0) {
        result = 6;
    }

cleanup:
    if (file.fd >= 0 && galay_c_aio_file_close(&file).code != C_IOResultOk && result == 0) {
        result = 7;
    }
    free(write_a);
    free(write_b);
    free(read_a);
    free(read_b);
    if (unlink(path) != 0 && errno != ENOENT && result == 0) {
        result = 8;
    }
    return result;
}
