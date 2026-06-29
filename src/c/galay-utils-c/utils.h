#ifndef GALAY_C_UTILS_UTILS_H
#define GALAY_C_UTILS_UTILS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef galay_status_t galay_utils_status_t;

#define GALAY_UTILS_OK GALAY_OK
#define GALAY_UTILS_INVALID_ARGUMENT GALAY_INVALID_ARGUMENT
#define GALAY_UTILS_BUFFER_TOO_SMALL GALAY_OUT_OF_MEMORY

typedef struct galay_utils_bytes_t galay_utils_bytes_t;
typedef struct galay_utils_ring_buffer_t galay_utils_ring_buffer_t;

const char* galay_utils_get_error(galay_status_t status);

galay_status_t galay_utils_bytes_create(const void* data, size_t len,
                                        galay_utils_bytes_t** out);
void galay_utils_bytes_destroy(galay_utils_bytes_t** bytes);
const void* galay_utils_bytes_data(const galay_utils_bytes_t* bytes);
size_t galay_utils_bytes_size(const galay_utils_bytes_t* bytes);
size_t galay_utils_bytes_capacity(const galay_utils_bytes_t* bytes);

galay_status_t galay_utils_ring_buffer_create(size_t capacity,
                                              galay_utils_ring_buffer_t** out);
void galay_utils_ring_buffer_destroy(galay_utils_ring_buffer_t** ring);
size_t galay_utils_ring_buffer_capacity(const galay_utils_ring_buffer_t* ring);
size_t galay_utils_ring_buffer_readable(const galay_utils_ring_buffer_t* ring);
size_t galay_utils_ring_buffer_writable(const galay_utils_ring_buffer_t* ring);
galay_status_t galay_utils_ring_buffer_write(galay_utils_ring_buffer_t* ring,
                                             const void* data, size_t len,
                                             size_t* actual);
galay_status_t galay_utils_ring_buffer_read(galay_utils_ring_buffer_t* ring,
                                            void* out, size_t len,
                                            size_t* actual);

galay_status_t galay_utils_base64_encode(const void* data, size_t len, char* out,
                                         size_t out_len, size_t* actual);
galay_status_t galay_utils_base64_decode(const char* data, size_t len, void* out,
                                         size_t out_len, size_t* actual);
galay_status_t galay_utils_md5(const void* data, size_t len, void* out,
                               size_t out_len);
galay_status_t galay_utils_sha1(const void* data, size_t len, void* out,
                                size_t out_len);
galay_status_t galay_utils_murmur3_32(const void* data, size_t len,
                                      uint32_t seed, uint32_t* out);

#ifdef __cplusplus
}
#endif

#endif
