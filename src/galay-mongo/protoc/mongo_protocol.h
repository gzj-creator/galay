/**
 * @file mongo_protocol.h
 * @brief MongoDB OP_MSG 协议编解码及命令构造
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 实现 MongoDB Wire Protocol 中 OP_MSG 消息格式的编解码，包括：
 * - MongoMessageHeader：16 字节消息头结构
 * - MongoMessage：解码后的完整消息
 * - MongoProtocol：OP_MSG 编解码与命令构造工具类
 *
 * @note 当前仅支持 OP_MSG（opcode 2013），不支持旧版 OP_QUERY/OP_REPLY
 */

#ifndef GALAY_MONGO_PROTOCOL_H
#define GALAY_MONGO_PROTOCOL_H

#include "bson.h"
#include "galay-mongo/base/mongo_error.h"
#include "galay-mongo/base/mongo_value.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace galay::mongo::protocol
{

constexpr int32_t kMongoOpReply = 1;          ///< OP_REPLY（旧版响应，已弃用）
constexpr int32_t kMongoOpCompressed = 2012;  ///< OP_COMPRESSED 压缩消息
constexpr int32_t kMongoOpMsg = 2013;         ///< OP_MSG 标准消息格式

/**
 * @brief MongoDB 消息头（16 字节）
 * @details 包含消息长度、请求 ID、响应关联 ID 和操作码
 */
struct MongoMessageHeader
{
    int32_t message_length = 0;  ///< 消息总长度（含头部）
    int32_t request_id = 0;      ///< 请求 ID
    int32_t response_to = 0;     ///< 对应的请求 ID（响应时使用）
    int32_t op_code = 0;         ///< 操作码
};

/**
 * @brief 解码后的 MongoDB 消息
 * @details 包含消息头、OP_MSG 标志位和消息体文档
 */
struct MongoMessage
{
    MongoMessageHeader header;   ///< 消息头
    int32_t flags = 0;           ///< OP_MSG 标志位
    MongoDocument body;          ///< 消息体（Section Kind 0）
};

/**
 * @brief MongoDB OP_MSG 协议编解码及命令构造工具
 * @details 提供静态方法用于 OP_MSG 的编码、解码和常用命令文档的构造
 */
class MongoProtocol
{
public:
    /**
     * @brief 将命令文档编码为 OP_MSG 二进制数据
     * @param request_id 请求 ID
     * @param body       命令文档
     * @param flags      OP_MSG 标志位，默认 0
     * @return OP_MSG 二进制字符串
     */
    static std::string encodeOpMsg(int32_t request_id,
                                   const MongoDocument& body,
                                   int32_t flags = 0);

    /**
     * @brief 将命令文档追加编码到已有字符串缓冲区尾部
     * @param out 输出缓冲区
     * @param request_id 请求 ID
     * @param body 命令文档
     * @param flags OP_MSG 标志位
     * @note 减少临时字符串分配，适合批量编码场景
     */
    static void appendOpMsg(std::string& out,
                            int32_t request_id,
                            const MongoDocument& body,
                            int32_t flags = 0);

    /**
     * @brief 将命令文档追加编码为 OP_MSG；若命令缺少 `$db` 字段则按需补齐
     * @param out 输出缓冲区
     * @param request_id 请求 ID
     * @param body 命令文档
     * @param database 默认数据库名
     * @param flags OP_MSG 标志位
     */
    static void appendOpMsgWithDatabase(std::string& out,
                                        int32_t request_id,
                                        const MongoDocument& body,
                                        std::string_view database,
                                        int32_t flags = 0);

    /**
     * @brief 从完整消息二进制数据解码为 MongoMessage
     * @param data 数据指针
     * @param len  数据长度
     * @return 解码后的消息，或 MongoError
     */
    static std::expected<MongoMessage, MongoError> decodeMessage(const char* data, size_t len);

    /**
     * @brief 从数据流中提取一条完整消息（带消费字节数输出）
     * @param data     数据指针
     * @param len      可用数据长度
     * @param consumed [out] 实际消费的字节数
     * @return 解码后的消息，或 MongoError
     */
    static std::expected<MongoMessage, MongoError> extractMessage(const char* data,
                                                                  size_t len,
                                                                  size_t& consumed);

    /**
     * @brief 构造 MongoDB 命令文档
     * @param db            目标数据库名
     * @param command_name  命令名称（如 "find", "insert"）
     * @param command_value 命令值（通常为集合名）
     * @param arguments     附加参数
     * @return 构造好的命令文档
     */
    static MongoDocument makeCommand(std::string db,
                                     std::string command_name,
                                     MongoValue command_value,
                                     MongoDocument arguments = {});
};

} // namespace galay::mongo::protocol

#endif // GALAY_MONGO_PROTOCOL_H
