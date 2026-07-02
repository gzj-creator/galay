/**
 * @file crc32c.h
 * @brief MongoDB wire protocol CRC32C checksum helper
 * @author galay-mongo
 * @version 1.0.0
 */

#ifndef GALAY_MONGO_PROTOCOL_CRC32C_H
#define GALAY_MONGO_PROTOCOL_CRC32C_H

#include <cstddef>
#include <cstdint>

namespace galay::mongo::protocol::detail
{

/**
 * @brief 计算 MongoDB OP_MSG 校验使用的 CRC32C(Castagnoli)。
 * @param data 输入数据指针；当 len 为 0 时可为空。
 * @param len  输入数据字节数。
 * @return CRC32C 校验值，使用初始值 0xFFFFFFFF 和最终取反。
 */
uint32_t crc32c(const char* data, size_t len);

} // namespace galay::mongo::protocol::detail

#endif // GALAY_MONGO_PROTOCOL_CRC32C_H
