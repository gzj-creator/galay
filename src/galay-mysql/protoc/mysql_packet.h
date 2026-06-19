/**
 * @file mysql_packet.h
 * @brief MySQL协议数据包结构与常量定义
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 定义了MySQL客户端/服务端协议中的所有数据包结构、命令类型、
 *          能力标志、服务器状态标志、字符集枚举以及解析错误码。
 */

#ifndef GALAY_MYSQL_PACKET_H
#define GALAY_MYSQL_PACKET_H

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include "galay-mysql/base/mysql_value.h"

namespace galay::mysql::protocol
{

// ======================== MySQL协议常量 ========================

constexpr uint32_t MYSQL_PACKET_HEADER_SIZE = 4;    ///< MySQL包头大小（4字节）
constexpr uint32_t MYSQL_MAX_PACKET_SIZE = 0xFFFFFF; ///< MySQL单包最大大小（16MB - 1）

/**
 * @brief MySQL命令类型枚举
 * @details 对应MySQL协议中的命令类型字节
 */
enum class CommandType : uint8_t
{
    COM_SLEEP               = 0x00, ///< 休眠
    COM_QUIT                = 0x01, ///< 退出连接
    COM_INIT_DB             = 0x02, ///< 初始化数据库
    COM_QUERY               = 0x03, ///< 执行查询
    COM_FIELD_LIST          = 0x04, ///< 列出字段
    COM_CREATE_DB           = 0x05, ///< 创建数据库
    COM_DROP_DB             = 0x06, ///< 删除数据库
    COM_REFRESH             = 0x07, ///< 刷新
    COM_SHUTDOWN            = 0x08, ///< 关闭服务器
    COM_STATISTICS          = 0x09, ///< 获取统计信息
    COM_PROCESS_INFO        = 0x0a, ///< 获取进程信息
    COM_CONNECT             = 0x0b, ///< 连接（内部）
    COM_PROCESS_KILL        = 0x0c, ///< 杀死进程
    COM_DEBUG               = 0x0d, ///< 调试
    COM_PING                = 0x0e, ///< 心跳检测
    COM_CHANGE_USER         = 0x11, ///< 切换用户
    COM_RESET_CONNECTION    = 0x1f, ///< 重置连接
    COM_STMT_PREPARE        = 0x16, ///< 准备预处理语句
    COM_STMT_EXECUTE        = 0x17, ///< 执行预处理语句
    COM_STMT_SEND_LONG_DATA = 0x18, ///< 发送长数据
    COM_STMT_CLOSE          = 0x19, ///< 关闭预处理语句
    COM_STMT_RESET          = 0x1a, ///< 重置预处理语句
};

/**
 * @brief MySQL能力标志位
 * @details 客户端和服务端握手时交换的能力标志
 */
enum CapabilityFlags : uint32_t
{
    CLIENT_LONG_PASSWORD                  = 0x00000001, ///< 使用长密码
    CLIENT_FOUND_ROWS                     = 0x00000002, ///< 返回匹配行数而非影响行数
    CLIENT_LONG_FLAG                      = 0x00000004, ///< 支持长标志
    CLIENT_CONNECT_WITH_DB                = 0x00000008, ///< 连接时指定数据库
    CLIENT_NO_SCHEMA                      = 0x00000010, ///< 不使用schema
    CLIENT_COMPRESS                       = 0x00000020, ///< 支持压缩协议
    CLIENT_ODBC                           = 0x00000040, ///< ODBC客户端
    CLIENT_LOCAL_FILES                    = 0x00000080, ///< 支持LOAD DATA LOCAL
    CLIENT_IGNORE_SPACE                   = 0x00000100, ///< 忽略函数名后的空格
    CLIENT_PROTOCOL_41                    = 0x00000200, ///< 使用4.1协议
    CLIENT_INTERACTIVE                    = 0x00000400, ///< 交互式客户端
    CLIENT_SSL                            = 0x00000800, ///< 支持SSL
    CLIENT_IGNORE_SIGPIPE                 = 0x00001000, ///< 忽略SIGPIPE
    CLIENT_TRANSACTIONS                   = 0x00002000, ///< 支持事务
    CLIENT_RESERVED                       = 0x00004000, ///< 保留
    CLIENT_SECURE_CONNECTION              = 0x00008000, ///< 安全连接
    CLIENT_MULTI_STATEMENTS               = 0x00010000, ///< 支持多语句
    CLIENT_MULTI_RESULTS                  = 0x00020000, ///< 支持多结果集
    CLIENT_PS_MULTI_RESULTS               = 0x00040000, ///< 预处理语句支持多结果集
    CLIENT_PLUGIN_AUTH                    = 0x00080000, ///< 支持插件认证
    CLIENT_CONNECT_ATTRS                  = 0x00100000, ///< 连接属性
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 0x00200000, ///< 插件认证使用长度编码
    CLIENT_DEPRECATE_EOF                  = 0x01000000, ///< 废弃EOF包
};

/**
 * @brief 服务器状态标志位
 * @details 在OK/EOF包中携带的服务器状态信息
 */
enum ServerStatusFlags : uint16_t
{
    SERVER_STATUS_IN_TRANS             = 0x0001, ///< 事务中
    SERVER_STATUS_AUTOCOMMIT           = 0x0002, ///< 自动提交模式
    SERVER_MORE_RESULTS_EXISTS         = 0x0008, ///< 还有更多结果
    SERVER_STATUS_NO_GOOD_INDEX_USED   = 0x0010, ///< 未使用好的索引
    SERVER_STATUS_NO_INDEX_USED        = 0x0020, ///< 未使用索引
    SERVER_STATUS_CURSOR_EXISTS        = 0x0040, ///< 游标存在
    SERVER_STATUS_LAST_ROW_SENT        = 0x0080, ///< 最后一行已发送
    SERVER_STATUS_DB_DROPPED           = 0x0100, ///< 数据库已删除
    SERVER_STATUS_NO_BACKSLASH_ESCAPES = 0x0200, ///< 禁用反斜杠转义
    SERVER_STATUS_METADATA_CHANGED     = 0x0400, ///< 元数据已变更
    SERVER_QUERY_WAS_SLOW              = 0x0800, ///< 查询执行缓慢
    SERVER_PS_OUT_PARAMS               = 0x1000, ///< 预处理语句输出参数
    SERVER_STATUS_IN_TRANS_READONLY    = 0x2000, ///< 只读事务
    SERVER_SESSION_STATE_CHANGED       = 0x4000, ///< 会话状态已变更
};

/**
 * @brief MySQL字符集枚举
 * @details 常用字符集的编号映射
 */
enum CharacterSet : uint8_t
{
    CHARSET_UTF8_GENERAL_CI    = 33,  ///< utf8_general_ci
    CHARSET_UTF8MB4_GENERAL_CI = 45,  ///< utf8mb4_general_ci
    CHARSET_UTF8MB4_0900_AI_CI = 255, ///< utf8mb4_0900_ai_ci (MySQL 8.0默认)
    CHARSET_BINARY             = 63,  ///< binary
};

// ======================== 数据包结构 ========================

/**
 * @brief MySQL包头
 * @details 每个MySQL包的固定4字节头部
 */
struct PacketHeader
{
    uint32_t length = 0;       ///< 3字节payload长度
    uint8_t sequence_id = 0;   ///< 序列号
};

/**
 * @brief 握手包 (HandshakeV10)
 * @details 服务器在连接建立后发送的初始握手包
 */
struct HandshakeV10
{
    uint8_t protocol_version = 0;        ///< 协议版本
    std::string server_version;          ///< 服务器版本字符串
    uint32_t connection_id = 0;          ///< 连接ID
    std::string auth_plugin_data;        ///< 认证插件salt数据（20字节）
    uint32_t capability_flags = 0;       ///< 服务器能力标志
    uint8_t character_set = 0;           ///< 默认字符集
    uint16_t status_flags = 0;           ///< 服务器状态标志
    std::string auth_plugin_name;        ///< 认证插件名称
};

/**
 * @brief 认证响应包 (HandshakeResponse41)
 * @details 客户端收到握手包后发送的认证响应
 */
struct HandshakeResponse41
{
    uint32_t capability_flags = 0;       ///< 客户端能力标志
    uint32_t max_packet_size = MYSQL_MAX_PACKET_SIZE; ///< 最大包大小
    uint8_t character_set = CHARSET_UTF8MB4_GENERAL_CI; ///< 字符集
    std::string username;                ///< 用户名
    std::string auth_response;           ///< 认证响应数据
    std::string database;                ///< 初始数据库
    std::string auth_plugin_name;        ///< 认证插件名称
};

/**
 * @brief 认证插件切换请求包
 * @details 服务端可在初始认证响应后请求客户端切换认证插件。
 */
struct AuthSwitchRequest
{
    std::string auth_plugin_name;        ///< 认证插件名称
    std::string auth_plugin_data;        ///< 认证插件salt数据
};

/**
 * @brief OK包
 * @details 表示操作成功的响应包
 */
struct OkPacket
{
    uint64_t affected_rows = 0;  ///< 影响行数
    uint64_t last_insert_id = 0; ///< 最后插入ID
    uint16_t status_flags = 0;   ///< 服务器状态标志
    uint16_t warnings = 0;       ///< 警告数
    std::string info;            ///< 附加信息
};

/**
 * @brief ERR包
 * @details 表示操作失败的错误响应包
 */
struct ErrPacket
{
    uint16_t error_code = 0;    ///< 错误码
    std::string sql_state;      ///< SQL状态码（5字节）
    std::string error_message;  ///< 错误消息
};

/**
 * @brief EOF包
 * @details 标记结果集某部分结束的包（MySQL 5.7.5以下使用）
 */
struct EofPacket
{
    uint16_t warnings = 0;      ///< 警告数
    uint16_t status_flags = 0;  ///< 服务器状态标志
};

/**
 * @brief 列定义包
 * @details 结果集中每列的元数据定义
 */
struct ColumnDefinitionPacket
{
    std::string catalog;        ///< 目录名
    std::string schema;         ///< 数据库名
    std::string table;          ///< 表名
    std::string org_table;      ///< 原始表名
    std::string name;           ///< 列别名
    std::string org_name;       ///< 列原始名
    uint16_t character_set = 0; ///< 字符集
    uint32_t column_length = 0; ///< 列长度
    uint8_t column_type = 0;    ///< 列类型
    uint16_t flags = 0;         ///< 列标志
    uint8_t decimals = 0;       ///< 小数位数
};

/**
 * @brief 结果集包（解析后的完整结果）
 * @details 包含列定义和行数据的完整结果集
 */
struct ResultSetPacket
{
    uint64_t column_count = 0;                              ///< 列数
    std::vector<ColumnDefinitionPacket> columns;            ///< 列定义列表
    std::vector<std::vector<std::optional<std::string>>> rows; ///< 行数据（每行每列的值）
    uint16_t status_flags = 0;                              ///< 服务器状态标志（来自最后的OK/EOF包）
    uint16_t warnings = 0;                                  ///< 警告数
};

/**
 * @brief COM_STMT_PREPARE响应
 * @details 预处理语句准备成功的响应包
 */
struct StmtPrepareOkPacket
{
    uint32_t statement_id = 0;                              ///< 语句ID
    uint16_t num_columns = 0;                               ///< 列数
    uint16_t num_params = 0;                                ///< 参数数
    uint16_t warning_count = 0;                             ///< 警告数
    std::vector<ColumnDefinitionPacket> param_defs;         ///< 参数定义列表
    std::vector<ColumnDefinitionPacket> column_defs;        ///< 列定义列表
};

/**
 * @brief 响应包类型标识
 * @details 通过payload首字节判断响应类型
 */
enum class ResponseType : uint8_t
{
    OK          = 0x00, ///< OK响应
    ERR         = 0xFF, ///< 错误响应
    EOF_PKT     = 0xFE, ///< EOF标记
    LOCAL_INFILE = 0xFB, ///< 本地文件请求
};

// ======================== 解析错误 ========================

/**
 * @brief 解析错误枚举
 * @details MySQL协议包解析过程中可能遇到的错误
 */
enum class ParseError
{
    Success,        ///< 解析成功
    Incomplete,     ///< 数据不完整
    InvalidFormat,  ///< 格式无效
    InvalidType,    ///< 类型无效
    InvalidLength,  ///< 长度无效
    BufferOverflow, ///< 缓冲区溢出
};

} // namespace galay::mysql::protocol

#endif // GALAY_MYSQL_PACKET_H
