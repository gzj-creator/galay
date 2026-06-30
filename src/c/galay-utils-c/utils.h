#ifndef GALAY_C_UTILS_UTILS_H
#define GALAY_C_UTILS_UTILS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

/**
 * @file utils.h
 * @brief Galay utils 模块的 C ABI。
 *
 * @details 该头文件提供字节数组、环形缓冲区和常用编码/摘要函数。除 create
 * 函数返回的 opaque handle 外，所有输入/输出缓冲区都由调用方拥有；函数不会
 * 保存调用方提供的临时输出缓冲区指针。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief utils 模块状态码。
 *
 * @details 当前 utils 模块直接复用 galay_status_t。GALAY_UTILS_BUFFER_TOO_SMALL
 * 是 GALAY_OUT_OF_MEMORY 的别名，用于表达调用方输出缓冲区容量不足。
 */
typedef galay_status_t galay_utils_status_t;

#define GALAY_UTILS_OK GALAY_OK                              ///< 操作成功。
#define GALAY_UTILS_INVALID_ARGUMENT GALAY_INVALID_ARGUMENT  ///< 参数非法。
#define GALAY_UTILS_BUFFER_TOO_SMALL GALAY_OUT_OF_MEMORY     ///< 输出缓冲区容量不足。

/**
 * @brief 不可变字节数组句柄。
 *
 * @note 由 galay_utils_bytes_create 创建，由 galay_utils_bytes_destroy 销毁。
 * 调用方不能解引用该 opaque 类型。
 */
typedef struct galay_utils_bytes_t galay_utils_bytes_t;

/**
 * @brief 单线程环形缓冲区句柄。
 *
 * @note 由 galay_utils_ring_buffer_create 创建，由 galay_utils_ring_buffer_destroy
 * 销毁。该类型不提供内部同步；多个线程并发访问时调用方必须自行串行化。
 */
typedef struct galay_utils_ring_buffer_t galay_utils_ring_buffer_t;

/**
 * @brief 将 utils 状态码转换为可读错误字符串。
 *
 * @param status utils/common 状态码。
 * @return 指向静态只读字符串的指针，调用方不得释放。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
const char* galay_utils_get_error(galay_status_t status);

/**
 * @brief 创建字节数组并复制输入数据。
 *
 * @param data 输入字节首地址；len 为 0 时可为 NULL。
 * @param len 输入字节数。
 * @param out 输出句柄地址；成功时写入新句柄，调用方负责 destroy。
 * @return 成功返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；分配失败返回
 * GALAY_OUT_OF_MEMORY。
 *
 * @note 该函数会复制 data 指向的内容，返回后调用方可立即释放或复用 data。
 * out 本身必须非 NULL；失败时 out 的内容不保证保持原值。
 */
galay_status_t galay_utils_bytes_create(const void* data, size_t len,
                                        galay_utils_bytes_t** out);

/**
 * @brief 销毁字节数组句柄。
 *
 * @param bytes 指向句柄的地址；可传 NULL 或指向 NULL 句柄。
 *
 * @note 成功释放后会将 *bytes 置为 NULL。该函数不会阻塞，不返回错误；调用方
 * 必须保证没有其它线程或读者继续使用该句柄。
 */
void galay_utils_bytes_destroy(galay_utils_bytes_t** bytes);

/**
 * @brief 获取字节数组内部只读数据指针。
 *
 * @param bytes 字节数组句柄；可为 NULL。
 * @return 非空数组返回内部数据指针；NULL 句柄或空数组返回 NULL。
 *
 * @note 返回指针由 bytes 拥有，在 bytes 被销毁或未来 ABI 扩展修改内容前有效。
 * 调用方不得写入或释放该指针。
 */
const void* galay_utils_bytes_data(const galay_utils_bytes_t* bytes);

/**
 * @brief 获取字节数组当前大小。
 *
 * @param bytes 字节数组句柄；可为 NULL。
 * @return bytes 为 NULL 时返回 0，否则返回字节数。
 */
size_t galay_utils_bytes_size(const galay_utils_bytes_t* bytes);

/**
 * @brief 获取字节数组当前容量。
 *
 * @param bytes 字节数组句柄；可为 NULL。
 * @return bytes 为 NULL 时返回 0，否则返回内部存储容量。
 *
 * @note 容量只用于诊断或预估，不是稳定 ABI 承诺。
 */
size_t galay_utils_bytes_capacity(const galay_utils_bytes_t* bytes);

/**
 * @brief 创建固定容量环形缓冲区。
 *
 * @param capacity 缓冲区容量，必须大于 0。
 * @param out 输出句柄地址；成功时写入新句柄，调用方负责 destroy。
 * @return 成功返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；分配失败返回
 * GALAY_OUT_OF_MEMORY。
 *
 * @note 环形缓冲区不拥有写入数据源，只把字节复制进内部存储。
 */
galay_status_t galay_utils_ring_buffer_create(size_t capacity,
                                              galay_utils_ring_buffer_t** out);

/**
 * @brief 销毁环形缓冲区。
 *
 * @param ring 指向 ring 句柄的地址；可传 NULL 或指向 NULL 句柄。
 *
 * @note 成功释放后会将 *ring 置为 NULL。调用方必须保证没有并发读写或悬挂指针。
 */
void galay_utils_ring_buffer_destroy(galay_utils_ring_buffer_t** ring);

/**
 * @brief 查询环形缓冲区总容量。
 *
 * @param ring ring 句柄；可为 NULL。
 * @return ring 为 NULL 时返回 0，否则返回创建时指定的容量。
 */
size_t galay_utils_ring_buffer_capacity(const galay_utils_ring_buffer_t* ring);

/**
 * @brief 查询当前可读字节数。
 *
 * @param ring ring 句柄；可为 NULL。
 * @return ring 为 NULL 时返回 0，否则返回当前已写入且未读取的字节数。
 */
size_t galay_utils_ring_buffer_readable(const galay_utils_ring_buffer_t* ring);

/**
 * @brief 查询当前可写字节数。
 *
 * @param ring ring 句柄；可为 NULL。
 * @return ring 为 NULL 时返回 0，否则返回剩余可写容量。
 */
size_t galay_utils_ring_buffer_writable(const galay_utils_ring_buffer_t* ring);

/**
 * @brief 向环形缓冲区写入字节。
 *
 * @param ring ring 句柄。
 * @param data 待写入字节；len 为 0 时可为 NULL。
 * @param len 待写入字节数。
 * @param actual 输出实际写入字节数；必须非 NULL，进入函数后会先置 0。
 * @return 成功完整写入返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；剩余
 * 容量不足返回 GALAY_OUT_OF_MEMORY。
 *
 * @note 该函数不会部分写入：容量不足时 actual 保持 0，ring 内容不应被调用方
 * 视为已完成写入。该函数不提供内部线程同步。
 */
galay_status_t galay_utils_ring_buffer_write(galay_utils_ring_buffer_t* ring,
                                             const void* data, size_t len,
                                             size_t* actual);

/**
 * @brief 从环形缓冲区读取字节。
 *
 * @param ring ring 句柄。
 * @param out 调用方提供的输出缓冲区；len 为 0 时可为 NULL。
 * @param len 待读取字节数。
 * @param actual 输出实际读取字节数；必须非 NULL，进入函数后会先置 0。
 * @return 成功完整读取返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；可读
 * 数据不足返回 GALAY_OUT_OF_MEMORY。
 *
 * @note 该函数不会部分读取：数据不足时 actual 保持 0。成功读取后对应字节会从
 * ring 中消费。
 */
galay_status_t galay_utils_ring_buffer_read(galay_utils_ring_buffer_t* ring,
                                            void* out, size_t len,
                                            size_t* actual);

/**
 * @brief Base64 编码。
 *
 * @param data 输入字节；len 为 0 时可为 NULL。
 * @param len 输入字节数。
 * @param out 调用方提供的输出字符缓冲区，必须非 NULL。
 * @param out_len 输出缓冲区容量。
 * @param actual 输出所需/实际字符数，必须非 NULL。
 * @return 成功返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；out_len 不足
 * 返回 GALAY_OUT_OF_MEMORY。
 *
 * @note actual 会写入不含结尾 '\0' 的 Base64 字符数。该函数不追加字符串终止符，
 * 调用方如需 C 字符串需额外预留并自行写入。
 */
galay_status_t galay_utils_base64_encode(const void* data, size_t len, char* out,
                                         size_t out_len, size_t* actual);

/**
 * @brief Base64 解码。
 *
 * @param data Base64 输入字符，长度必须为 4 的倍数。
 * @param len 输入字符数。
 * @param out 调用方提供的输出字节缓冲区，必须非 NULL。
 * @param out_len 输出缓冲区容量。
 * @param actual 输出所需/实际字节数，必须非 NULL。
 * @return 成功返回 GALAY_OK；参数非法、长度非法或包含非法字符时返回
 * GALAY_INVALID_ARGUMENT；out_len 不足返回 GALAY_OUT_OF_MEMORY。
 *
 * @note actual 会写入解码后的字节数。该函数不拥有 data/out，不提供内部同步。
 */
galay_status_t galay_utils_base64_decode(const char* data, size_t len, void* out,
                                         size_t out_len, size_t* actual);

/**
 * @brief 计算 MD5 摘要。
 *
 * @param data 输入字节；len 为 0 时可为 NULL。
 * @param len 输入字节数。
 * @param out 调用方提供的输出缓冲区，至少 16 字节。
 * @param out_len 输出缓冲区容量。
 * @return 成功返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；out_len 小于
 * 16 返回 GALAY_OUT_OF_MEMORY。
 *
 * @note 输出为原始 16 字节 digest，不是十六进制字符串。
 */
galay_status_t galay_utils_md5(const void* data, size_t len, void* out,
                               size_t out_len);

/**
 * @brief 计算 SHA1 摘要。
 *
 * @param data 输入字节；len 为 0 时可为 NULL。
 * @param len 输入字节数。
 * @param out 调用方提供的输出缓冲区，至少 20 字节。
 * @param out_len 输出缓冲区容量。
 * @return 成功返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT；out_len 小于
 * 20 返回 GALAY_OUT_OF_MEMORY。
 *
 * @note 输出为原始 20 字节 digest，不是十六进制字符串。
 */
galay_status_t galay_utils_sha1(const void* data, size_t len, void* out,
                                size_t out_len);

/**
 * @brief 计算 MurmurHash3 32-bit 哈希。
 *
 * @param data 输入字节；len 为 0 时可为 NULL。
 * @param len 输入字节数。
 * @param seed 哈希种子。
 * @param out 输出哈希值地址，必须非 NULL。
 * @return 成功返回 GALAY_OK；参数非法返回 GALAY_INVALID_ARGUMENT。
 *
 * @note out 使用主机字节序写入 uint32_t。该函数不保存 data 指针。
 */
galay_status_t galay_utils_murmur3_32(const void* data, size_t len,
                                      uint32_t seed, uint32_t* out);

#ifdef __cplusplus
}
#endif

#endif
