/**
 * @file mysql_protocol.h
 * @brief MySQL协议解析器与编码器
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 定义了MySQL协议的辅助读写函数（长度编码整数/字符串、固定长度整数等）、
 *          协议解析器(MysqlParser)和协议编码器(MysqlEncoder)。
 *          解析器支持解析握手包、OK包、ERR包、EOF包、列定义包、行数据等。
 *          编码器支持编码认证响应、查询命令、预处理语句命令等。
 */

#ifndef GALAY_MYSQL_PROTOCOL_H
#define GALAY_MYSQL_PROTOCOL_H

#include "mysql_packet.h"
#include "mysql_auth.h"
#include "../base/mysql_config.h"
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <expected>
#include <cstdint>
#include <optional>

namespace galay::mysql::protocol
{

// ======================== 辅助函数 ========================

/**
 * @brief 读取length-encoded integer
 * @param data 数据指针
 * @param len 可用长度
 * @param consumed 输出：消耗的字节数
 * @return 解析的整数值
 */
std::expected<uint64_t, ParseError> read_len_enc_int(const char* data, size_t len, size_t& consumed);

/**
 * @brief 读取length-encoded string
 * @param data 数据指针
 * @param len 可用长度
 * @param consumed 输出：消耗的字节数
 * @return 解析的字符串
 */
std::expected<std::string, ParseError> read_len_enc_string(const char* data, size_t len, size_t& consumed);

/**
 * @brief 读取length-encoded string并返回借用视图
 * @param data 数据指针
 * @param len 可用长度
 * @param consumed 输出：消耗的字节数
 * @return 指向输入缓冲区内部的字符串视图
 * @note 返回的string_view不拥有数据，仅在调用方提供的缓冲区保持存活且不被修改时有效。
 */
std::expected<std::string_view, ParseError> read_len_enc_string_view(const char* data, size_t len, size_t& consumed);

/**
 * @brief 读取null-terminated string
 * @param data 输入数据
 * @param len 数据字节数
 * @param consumed 接收已消费字节数的引用
 * @return 成功时返回 std::string，失败时返回 ParseError 错误
 */
std::expected<std::string, ParseError> read_null_term_string(const char* data, size_t len, size_t& consumed);

/**
 * @brief 读取固定长度整数（小端序）
 * @param data 输入数据
 * @return 从输入地址按小端序读取的 16 位整数
 */
uint16_t read_uint16(const char* data);
uint32_t read_uint24(const char* data);
uint32_t read_uint32(const char* data);
uint64_t read_uint64(const char* data);

/**
 * @brief 写入固定长度整数（小端序）
 * @param buf 数据缓冲区引用
 * @param val 值
 * @return 无返回值
 */
void write_uint16(std::string& buf, uint16_t val);
void write_uint24(std::string& buf, uint32_t val);
void write_uint32(std::string& buf, uint32_t val);
void write_uint64(std::string& buf, uint64_t val);

/**
 * @brief 写入length-encoded integer
 * @param buf 数据缓冲区引用
 * @param val 值
 * @return 无返回值
 */
void write_len_enc_int(std::string& buf, uint64_t val);

/**
 * @brief 写入length-encoded string
 * @param buf 数据缓冲区引用
 * @param str 待处理字符串
 * @return 无返回值
 */
void write_len_enc_string(std::string& buf, std::string_view str);

// ======================== 解析器 ========================

/**
 * @brief MySQL协议解析器
 * @details 负责解析MySQL协议的各种数据包，包括握手包、OK/ERR/EOF包、
 *          列定义包、行数据包以及预处理语句响应等。
 */
class MysqlParser
{
public:
    MysqlParser() = default;

    /**
     * @brief 解析包头
     * @param data 原始数据（至少4字节）
     * @param len 数据长度
     * @return PacketHeader 或 ParseError
     */
    std::expected<PacketHeader, ParseError> parse_header(const char* data, size_t len);

    /**
     * @brief 解析握手包
     * @param data payload数据（不含包头）
     * @param len payload长度
     * @return 成功时返回 HandshakeV10，失败时返回 ParseError 错误
     */
    std::expected<HandshakeV10, ParseError> parse_handshake(const char* data, size_t len);

    /**
     * @brief 解析AuthSwitchRequest包
     * @param data payload数据（不含包头，含0xFE标识字节）
     * @param len payload长度
     * @return 成功时返回 AuthSwitchRequest，失败时返回 ParseError 错误
     */
    std::expected<AuthSwitchRequest, ParseError> parse_auth_switch_request(const char* data, size_t len);

    /**
     * @brief 判断响应类型
     * @param first_byte payload的第一个字节
     * @param payload_len payload长度
     * @return 由首字节和负载长度识别出的响应类型
     */
    ResponseType identify_response(uint8_t first_byte, uint32_t payload_len);

    /**
     * @brief 解析OK包
     * @param data payload数据（不含包头，含0x00标识字节）
     * @param len payload长度
     * @param capabilities 客户端能力标志
     * @return 成功时返回 OkPacket，失败时返回 ParseError 错误
     */
    std::expected<OkPacket, ParseError> parse_ok(const char* data, size_t len, uint32_t capabilities);

    /**
     * @brief 解析ERR包
     * @param data payload数据（不含包头，含0xFF标识字节）
     * @param len payload长度
     * @param capabilities 客户端能力标志
     * @return 成功时返回 ErrPacket，失败时返回 ParseError 错误
     */
    std::expected<ErrPacket, ParseError> parse_err(const char* data, size_t len, uint32_t capabilities);

    /**
     * @brief 解析EOF包
     * @param data payload数据（不含包头，含0xFE标识字节）
     * @param len payload长度
     * @return 成功时返回 EofPacket，失败时返回 ParseError 错误
     */
    std::expected<EofPacket, ParseError> parse_eof(const char* data, size_t len);

    /**
     * @brief 解析列定义包
     * @param data payload数据（不含包头）
     * @param len payload长度
     * @return 成功时返回 ColumnDefinitionPacket，失败时返回 ParseError 错误
     */
    std::expected<ColumnDefinitionPacket, ParseError> parse_column_definition(const char* data, size_t len);

    /**
     * @brief 解析文本协议行数据
     * @param data payload数据（不含包头）
     * @param len payload长度
     * @param column_count 列数
     * @return 一行数据（每列为optional<string>，NULL用nullopt表示）
     */
    std::expected<std::vector<std::optional<std::string>>, ParseError>
    parse_text_row(const char* data, size_t len, size_t column_count);

    /**
     * @brief 解析文本协议行数据并返回借用视图
     * @param data payload数据（不含包头）
     * @param len payload长度
     * @param column_count 列数
     * @return 一行数据（每列为optional<string_view>，NULL用nullopt表示）
     * @note 返回的string_view不拥有数据，仅在调用方提供的payload缓冲区保持存活且不被修改时有效。
     */
    std::expected<std::vector<std::optional<std::string_view>>, ParseError>
    parse_text_row_view(const char* data, size_t len, size_t column_count);

    /**
     * @brief 解析COM_STMT_PREPARE响应的OK部分
     * @param data payload数据（不含包头）
     * @param len payload长度
     * @return 成功时返回 StmtPrepareOkPacket，失败时返回 ParseError 错误
     */
    std::expected<StmtPrepareOkPacket, ParseError> parse_stmt_prepare_ok(const char* data, size_t len);

    /**
     * @brief 从完整的缓冲区中解析一个完整的MySQL包
     * @param data 缓冲区数据
     * @param len 缓冲区长度
     * @param consumed 输出：消耗的总字节数（包头+payload）
     * @return payload数据的起始位置和长度
     */
    struct PacketView {
        const char* payload;     ///< payload数据指针
        uint32_t payload_len;    ///< payload长度
        uint8_t sequence_id;     ///< 序列号
    };
    std::expected<PacketView, ParseError> extract_packet(const char* data, size_t len, size_t& consumed);
};

// ======================== 编码器 ========================

/**
 * @brief MySQL协议编码器
 * @details 负责将客户端请求编码为MySQL协议格式的数据包，
 *          包括认证响应、查询命令、预处理语句命令等。
 */
class MysqlEncoder
{
public:
    MysqlEncoder() = default;

    /**
     * @brief 编码认证响应包
     * @param resp 认证响应数据
     * @param sequence_id 序列号
     * @return 完整的MySQL包（包头+payload）
     */
    std::string encode_handshake_response(const HandshakeResponse41& resp, uint8_t sequence_id);

    /**
     * @brief 编码COM_QUERY命令
     * @param sql SQL语句
     * @param sequence_id 序列号（通常为0）
     * @return 完整的MySQL包
     */
    std::string encode_query(std::string_view sql, uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_STMT_PREPARE命令
     * @param sql SQL语句
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string encode_stmt_prepare(std::string_view sql, uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_STMT_EXECUTE命令
     * @param stmt_id 语句ID
     * @param params 参数值（字符串形式）
     * @param param_types 参数类型
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string encode_stmt_execute(uint32_t stmt_id,
                                   std::span<const std::optional<std::string>> params,
                                   std::span<const uint8_t> param_types,
                                   uint8_t sequence_id = 0);
    std::string encode_stmt_execute(uint32_t stmt_id,
                                   std::span<const std::optional<std::string_view>> params,
                                   std::span<const uint8_t> param_types,
                                   uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_STMT_CLOSE命令
     * @param stmt_id 语句ID
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string encode_stmt_close(uint32_t stmt_id, uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_QUIT命令
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string encode_quit(uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_PING命令
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string encode_ping(uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_INIT_DB命令
     * @param database 数据库名
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string encode_init_db(std::string_view database, uint8_t sequence_id = 0);

    /**
     * @brief 编码COM_RESET_CONNECTION命令
     * @param sequence_id 序列号
     * @return 处理后的 std::string 结果
     */
    std::string encode_reset_connection(uint8_t sequence_id = 0);

private:
    /**
     * @brief 给payload添加包头
     * @param payload payload数据
     * @param sequence_id 序列号
     * @return 完整的MySQL包
     */
    std::string wrap_packet(std::string_view payload, uint8_t sequence_id);

    /**
     * @brief 编码简单命令（1字节命令 + 可选payload）
     * @param cmd 命令名称
     * @param payload 消息负载
     * @param sequence_id 序列号
     * @return 处理后的 std::string 结果
     */
    std::string encode_simple_command(CommandType cmd, std::string_view payload, uint8_t sequence_id);
};

} // namespace galay::mysql::protocol

#endif // GALAY_MYSQL_PROTOCOL_H
