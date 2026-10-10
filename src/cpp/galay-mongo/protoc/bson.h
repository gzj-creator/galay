/**
 * @file bson.h
 * @brief BSON 编解码器
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 实现 MongoDB BSON 二进制格式与 C++ MongoDocument 之间的编解码，
 * 包括：
 * - BsonType 枚举：BSON 元素类型标识
 * - BsonCodec 类：静态编解码方法集合
 *
 * @note 编码遵循 BSON 规范（https://bsonspec.org/）
 */

#ifndef GALAY_MONGO_BSON_H
#define GALAY_MONGO_BSON_H

#include "../base/mongo_value.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <string>

namespace galay::mongo::protocol
{

/**
 * @brief BSON 元素类型标识（对应 BSON 规范中的 type byte）
 */
enum class BsonType : uint8_t
{
    Double = 0x01,      ///< 64 位浮点数
    String = 0x02,      ///< UTF-8 字符串
    Document = 0x03,    ///< 嵌套文档
    Array = 0x04,       ///< 数组
    Binary = 0x05,      ///< 二进制数据
    ObjectId = 0x07,    ///< 12 字节 ObjectId
    Bool = 0x08,        ///< 布尔值
    DateTime = 0x09,    ///< UTC 日期时间（毫秒时间戳）
    Null = 0x0A,        ///< 空值
    Int32 = 0x10,       ///< 32 位整数
    Timestamp = 0x11,   ///< MongoDB 内部时间戳
    Int64 = 0x12,       ///< 64 位整数
};

/**
 * @brief BSON 编解码器，负责 MongoDocument 与 BSON 二进制格式之间的转换
 * @details 提供静态方法将 MongoDocument 编码为 BSON 二进制数据，以及从 BSON 二进制数据解码为 MongoDocument
 */
class BsonCodec
{
public:
    /**
     * @brief 将 MongoDocument 编码为 BSON 二进制数据
     * @param document 要编码的文档
     * @return 成功时返回 BSON 二进制字符串；失败时返回错误描述
     */
    static std::expected<std::string, std::string> encode_document(const MongoDocument& document);

    /**
     * @brief 将 MongoDocument 直接追加编码到现有缓冲区尾部
     * @param out 输出缓冲区
     * @param document 要编码的文档
     * @return 成功时返回空值；失败时返回错误描述且不保留本次追加的部分数据
     * @note 避免中间临时字符串分配，适合批量编码场景
     */
    static std::expected<void, std::string> append_document(std::string& out, const MongoDocument& document);

    /**
     * @brief 将 MongoDocument 追加编码到缓冲区；若原文档缺少 `$db` 字段则按需补齐
     * @param out 输出缓冲区
     * @param document 要编码的文档
     * @param database 默认数据库名，当文档无 `$db` 字段时使用
     * @return 成功时返回空值；失败时返回错误描述且不保留本次追加的部分数据
     */
    static std::expected<void, std::string> append_document_with_database(std::string& out,
                                                                       const MongoDocument& document,
                                                                       std::string_view database);

    /**
     * @brief 从 BSON 二进制数据解码为 MongoDocument
     * @param data 数据指针
     * @param len  数据长度
     * @return 解码后的文档，或错误描述
     */
    static std::expected<MongoDocument, std::string> decode_document(const char* data, size_t len);

    /**
     * @brief 从 BSON 二进制数据解码为 MongoDocument（带消费字节数输出）
     * @param data     数据指针
     * @param len      数据长度
     * @param consumed [out] 实际消费的字节数
     * @return 解码后的文档，或错误描述
     */
    static std::expected<MongoDocument, std::string> decode_document(const char* data,
                                                                    size_t len,
                                                                    size_t& consumed);

private:
    /**
     * @brief 写入 32 位整数（小端序）
     * @param out 追加编码字节的输出字符串
     * @param value 待设置或处理的值
     * @return 无返回值
     */
    static void write_int32(std::string& out, int32_t value);
    /**
     * @brief 写入 64 位整数（小端序）
     * @param out 追加编码字节的输出字符串
     * @param value 待设置或处理的值
     * @return 无返回值
     */
    static void write_int64(std::string& out, int64_t value);
    /**
     * @brief 写入双精度浮点数（小端序）
     * @param out 追加编码字节的输出字符串
     * @param value 待设置或处理的值
     * @return 无返回值
     */
    static void write_double(std::string& out, double value);
    /**
     * @brief 写入 C 风格字符串（无长度前缀）
     * @param out 追加字符串及终止符的输出字符串
     * @param value 待设置或处理的值
     * @return 成功时返回空值，失败时返回 std::string 错误
     */
    static std::expected<void, std::string> write_c_string(std::string& out, std::string_view value);

    /**
     * @brief 读取 32 位整数
     * @param data 输入数据
     * @param len 数据字节数
     * @param pos 位置
     * @return 成功时返回 int32_t，失败时返回 std::string 错误
     */
    static std::expected<int32_t, std::string> read_int32(const char* data, size_t len, size_t pos);
    /**
     * @brief 读取 64 位整数
     * @param data 输入数据
     * @param len 数据字节数
     * @param pos 位置
     * @return 成功时返回 int64_t，失败时返回 std::string 错误
     */
    static std::expected<int64_t, std::string> read_int64(const char* data, size_t len, size_t pos);
    /**
     * @brief 读取双精度浮点数
     * @param data 输入数据
     * @param len 数据字节数
     * @param pos 位置
     * @return 成功时返回 double，失败时返回 std::string 错误
     */
    static std::expected<double, std::string> read_double(const char* data, size_t len, size_t pos);
    /**
     * @brief 读取 C 风格字符串
     * @param data 输入数据
     * @param len 数据字节数
     * @param pos 位置
     * @return 成功时返回 std::string，失败时返回 std::string 错误
     */
    static std::expected<std::string, std::string> read_c_string(const char* data, size_t len, size_t& pos);

    /**
     * @brief 编码单个 BSON 元素
     * @param out 追加 BSON 元素编码字节的输出字符串
     * @param key 键
     * @param value 待设置或处理的值
     * @return 成功时返回空值，失败时返回 std::string 错误
     */
    static std::expected<void, std::string> encode_element(std::string& out, std::string_view key, const MongoValue& value);
    static std::expected<MongoValue, std::string> decode_element_value(BsonType type,               ///< 解码单个 BSON 元素值
                                                                      const char* data,
                                                                      size_t len,
                                                                      size_t& pos);
};

} // namespace galay::mongo::protocol

#endif // GALAY_MONGO_BSON_H
