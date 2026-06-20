/**
 * @file utils.h
 * @brief Galay utils 模块 C ABI
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 该头文件可被 C11 和 C++ 编译器直接包含。所有拥有所有权的
 *          对象均通过 opaque handle 暴露，销毁函数接收 handle 指针并置空。
 */

#ifndef GALAY_C_GALAY_UTILS_UTILS_H
#define GALAY_C_GALAY_UTILS_UTILS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief utils C API 状态码
 */
typedef enum galay_utils_status {
    GALAY_UTILS_OK = 0,
    GALAY_UTILS_INVALID_ARGUMENT = 1,
    GALAY_UTILS_OUT_OF_MEMORY = 2,
    GALAY_UTILS_BUFFER_TOO_SMALL = 3,
    GALAY_UTILS_INTERNAL_ERROR = 4
} galay_utils_status_t;

typedef struct galay_utils_bytes galay_utils_bytes_t;
typedef struct galay_utils_ring_buffer galay_utils_ring_buffer_t;

/**
 * @brief 创建拥有独立拷贝的字节对象
 * @param data 输入字节；length 为 0 时可为 NULL
 * @param length 输入字节数
 * @param out 输出 handle，成功后由调用方通过 galay_utils_bytes_destroy 释放
 * @return 状态码
 */
galay_utils_status_t galay_utils_bytes_create(
    const void* data,
    size_t length,
    galay_utils_bytes_t** out);

/**
 * @brief 销毁字节对象并将 handle 置空
 * @param bytes 指向 handle 的指针；NULL 或 *bytes 为 NULL 时无操作
 */
void galay_utils_bytes_destroy(galay_utils_bytes_t** bytes);

/**
 * @brief 获取字节对象中的只读数据指针
 * @param bytes 字节对象
 * @return 只读数据指针；空对象或 NULL handle 返回 NULL
 */
const uint8_t* galay_utils_bytes_data(const galay_utils_bytes_t* bytes);

/**
 * @brief 获取可读字节数
 * @param bytes 字节对象
 * @return 可读字节数；NULL handle 返回 0
 */
size_t galay_utils_bytes_size(const galay_utils_bytes_t* bytes);

/**
 * @brief 获取容量
 * @param bytes 字节对象
 * @return 容量；NULL handle 返回 0
 */
size_t galay_utils_bytes_capacity(const galay_utils_bytes_t* bytes);

/**
 * @brief 创建固定容量环形缓冲区
 * @param capacity 容量，必须大于 0
 * @param out 输出 handle，成功后由调用方通过 galay_utils_ring_buffer_destroy 释放
 * @return 状态码
 */
galay_utils_status_t galay_utils_ring_buffer_create(
    size_t capacity,
    galay_utils_ring_buffer_t** out);

/**
 * @brief 销毁环形缓冲区并将 handle 置空
 * @param ring 指向 handle 的指针；NULL 或 *ring 为 NULL 时无操作
 */
void galay_utils_ring_buffer_destroy(galay_utils_ring_buffer_t** ring);

size_t galay_utils_ring_buffer_capacity(const galay_utils_ring_buffer_t* ring);
size_t galay_utils_ring_buffer_readable(const galay_utils_ring_buffer_t* ring);
size_t galay_utils_ring_buffer_writable(const galay_utils_ring_buffer_t* ring);

/**
 * @brief 写入环形缓冲区
 * @param ring 环形缓冲区
 * @param data 输入字节；length 为 0 时可为 NULL
 * @param length 请求写入字节数
 * @param written 实际写入字节数；可为 NULL
 * @return 全量写入返回 OK，空间不足返回 BUFFER_TOO_SMALL
 */
galay_utils_status_t galay_utils_ring_buffer_write(
    galay_utils_ring_buffer_t* ring,
    const void* data,
    size_t length,
    size_t* written);

/**
 * @brief 从环形缓冲区读取并消费字节
 * @param ring 环形缓冲区
 * @param data 输出缓冲区；length 为 0 时可为 NULL
 * @param length 请求读取字节数
 * @param read_bytes 实际读取字节数；可为 NULL
 * @return 全量读取返回 OK，可读数据不足返回 BUFFER_TOO_SMALL
 */
galay_utils_status_t galay_utils_ring_buffer_read(
    galay_utils_ring_buffer_t* ring,
    void* data,
    size_t length,
    size_t* read_bytes);

void galay_utils_ring_buffer_clear(galay_utils_ring_buffer_t* ring);

/**
 * @brief Base64 编码
 * @param data 输入字节；length 为 0 时可为 NULL
 * @param length 输入字节数
 * @param output 输出缓冲区；output_capacity 必须至少为 *output_length
 * @param output_capacity 输出缓冲区容量
 * @param output_length 成功时为实际长度，缓冲区不足时为所需长度
 * @return 状态码
 */
galay_utils_status_t galay_utils_base64_encode(
    const void* data,
    size_t length,
    char* output,
    size_t output_capacity,
    size_t* output_length);

/**
 * @brief Base64 解码
 * @param data Base64 输入；length 为 0 时可为 NULL
 * @param length 输入字节数
 * @param output 输出缓冲区
 * @param output_capacity 输出缓冲区容量
 * @param output_length 成功时为实际长度，缓冲区不足时为所需长度
 * @return 状态码；非法 Base64 输入返回 INVALID_ARGUMENT
 */
galay_utils_status_t galay_utils_base64_decode(
    const char* data,
    size_t length,
    void* output,
    size_t output_capacity,
    size_t* output_length);

galay_utils_status_t galay_utils_md5(
    const void* data,
    size_t length,
    uint8_t* output,
    size_t output_capacity);

galay_utils_status_t galay_utils_sha1(
    const void* data,
    size_t length,
    uint8_t* output,
    size_t output_capacity);

galay_utils_status_t galay_utils_murmur3_32(
    const void* data,
    size_t length,
    uint32_t seed,
    uint32_t* output);

#ifdef __cplusplus
}
#endif

#endif
