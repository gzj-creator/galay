#ifndef GALAY_C_COMMON_GALAY_C_IOVEC_H
#define GALAY_C_COMMON_GALAY_C_IOVEC_H

#include <stddef.h>

/**
 * @file galay_c_iovec.h
 * @brief Galay C ABI 自有 scatter/gather buffer 描述。
 *
 * @details 公开 C ABI 使用该结构表达 iovec，避免语言绑定直接依赖 POSIX
 * `struct iovec`。实现层会在进入平台 I/O 前转换为后端需要的布局。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单段缓冲区描述。
 *
 * @details base 指向调用方拥有的缓冲区，len 为缓冲区字节数。
 *
 * @note [borrowed] 调用方必须保证 base 指向的内存在对应 API 返回前保持有效。
 * ABI 演进只能在尾部追加字段，禁止在现有字段之间插入或改变字段含义。
 */
typedef struct galay_iovec_t {
    void* base;    ///< [borrowed] 缓冲区起始地址。
    size_t len;    ///< 缓冲区字节数。
} galay_iovec_t;

#ifdef __cplusplus
}
#endif

#endif
