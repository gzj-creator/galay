/**
 * @file builder.h
 * @brief MySQL命令构建器
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 提供MySQL协议命令的批量编码构建功能，支持查询、预处理、Ping、Quit等命令，
 *          用于Pipeline模式的批量命令发送。
 */

#ifndef GALAY_MYSQL_PROTOCOL_BUILDER_H
#define GALAY_MYSQL_PROTOCOL_BUILDER_H

#include "mysql_protocol.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::mysql::protocol
{

/**
 * @brief MySQL命令类型枚举
 * @details 标识构建器中命令的语义类型，用于Pipeline模式下的命令分类。
 */
enum class MysqlCommandKind : uint8_t
{
    Raw = 0,            ///< 原始命令
    Query,              ///< 查询命令
    StmtPrepare,        ///< 预处理命令
    InitDb,             ///< 初始化数据库命令
    Ping,               ///< 心跳命令
    Quit,               ///< 退出命令
    ResetConnection     ///< 重置连接命令
};

/**
 * @brief MySQL命令视图
 * @details 指向编码缓冲区中某条命令的轻量视图，不持有数据。
 */
struct MysqlCommandView
{
    std::string_view encoded;              ///< 编码后的命令数据视图
    MysqlCommandKind kind = MysqlCommandKind::Raw; ///< 命令类型
    uint8_t sequence_id = 0;               ///< 序列号
};

/**
 * @brief 编码后的批量命令
 * @details 包含所有编码后的命令数据和预期响应数量。
 */
struct MysqlEncodedBatch
{
    MysqlEncodedBatch() = default;
    MysqlEncodedBatch(std::string encoded_data, size_t response_count) noexcept;
    MysqlEncodedBatch(MysqlEncodedBatch&&) noexcept = default;
    MysqlEncodedBatch& operator=(MysqlEncodedBatch&&) noexcept = default;

    /**
     * @brief 显式复制编码批量命令
     * @return 拥有独立编码缓冲区的新批量命令
     */
    [[nodiscard]] MysqlEncodedBatch clone() const;

    std::string encoded;                    ///< 编码后的命令数据
    size_t expected_responses = 0;          ///< 预期响应数量

private:
    MysqlEncodedBatch(const MysqlEncodedBatch&) = delete;
    MysqlEncodedBatch& operator=(const MysqlEncodedBatch&) = delete;
};

/**
 * @brief MySQL命令构建器
 * @details 支持链式调用，批量编码多条MySQL命令，用于Pipeline模式。
 *          内部维护一个编码缓冲区和命令元数据列表，按需生成命令视图。
 */
class MysqlCommandBuilder
{
public:
    MysqlCommandBuilder() = default;
    MysqlCommandBuilder(MysqlCommandBuilder&& other) noexcept;
    MysqlCommandBuilder& operator=(MysqlCommandBuilder&& other) noexcept;

    /**
     * @brief 显式复制命令构建器
     * @details 复制编码缓冲区和命令元数据，并丢弃旧视图缓存；下一次commands()
     *          会重新生成指向新缓冲区的string_view。
     * @return 拥有独立编码缓冲区的新构建器
     */
    [[nodiscard]] MysqlCommandBuilder clone() const;

    /**
     * @brief 清空所有已添加的命令
     * @return 无返回值
     */
    void clear() noexcept;

    /**
     * @brief 预分配空间
     * @param command_count 预期命令数量
     * @param encoded_bytes 预期编码总字节数
     * @return 无返回值
     */
    void reserve(size_t command_count, size_t encoded_bytes);

    /**
     * @brief 添加COM_QUERY命令
     * @param sql SQL语句
     * @param sequence_id 序列号
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_query(std::string_view sql, uint8_t sequence_id = 0);

    /**
     * @brief 添加COM_STMT_PREPARE命令
     * @param sql 预处理SQL语句
     * @param sequence_id 序列号
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_stmt_prepare(std::string_view sql, uint8_t sequence_id = 0);

    /**
     * @brief 添加COM_INIT_DB命令
     * @param database 数据库名
     * @param sequence_id 序列号
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_init_db(std::string_view database, uint8_t sequence_id = 0);

    /**
     * @brief 添加COM_PING命令
     * @param sequence_id 序列号
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_ping(uint8_t sequence_id = 0);

    /**
     * @brief 添加COM_QUIT命令
     * @param sequence_id 序列号
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_quit(uint8_t sequence_id = 0);

    /**
     * @brief 添加COM_RESET_CONNECTION命令
     * @param sequence_id 序列号
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_reset_connection(uint8_t sequence_id = 0);

    /**
     * @brief 添加简单命令
     * @param cmd 命令类型
     * @param payload 负载数据
     * @param sequence_id 序列号
     * @param kind 命令语义类型
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_simple(CommandType cmd,
                                      std::string_view payload = {},
                                      uint8_t sequence_id = 0,
                                      MysqlCommandKind kind = MysqlCommandKind::Raw);

    /**
     * @brief 快速添加命令（调用方保证已预留足够空间）
     * @param cmd 命令类型
     * @param payload 负载数据
     * @param sequence_id 序列号
     * @param kind 命令语义类型
     * @return 构建器引用
     */
    MysqlCommandBuilder& append_fast(CommandType cmd,
                                    std::string_view payload,
                                    uint8_t sequence_id = 0,
                                    MysqlCommandKind kind = MysqlCommandKind::Raw);

    /**
     * @brief 获取命令视图数组
     * @return 命令视图的span
     */
    [[nodiscard]] std::span<const MysqlCommandView> commands() const;

    /**
     * @brief 获取命令数量
     * @return 命令数量
     */
    [[nodiscard]] size_t size() const noexcept;

    /**
     * @brief 判断是否为空
     * @return 为空时返回true
     */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * @brief 获取编码缓冲区
     * @return 编码数据的常量引用
     */
    [[nodiscard]] const std::string& encoded() const noexcept;

    /**
     * @brief 构建批量命令（拷贝版本）
     * @return 编码后的批量命令
     */
    [[nodiscard]] MysqlEncodedBatch build() const;

    /**
     * @brief 构建并释放批量命令（移动版本）
     * @return 编码后的批量命令
     */
    [[nodiscard]] MysqlEncodedBatch release();

private:
    /**
     * @brief 编码命令切片
     */
    struct Slice
    {
        size_t offset = 0;  ///< 在编码缓冲区中的偏移量
        size_t length = 0;  ///< 切片长度
    };

    /**
     * @brief 命令元数据
     */
    struct CommandMeta
    {
        Slice encoded;                               ///< 编码切片
        MysqlCommandKind kind = MysqlCommandKind::Raw; ///< 命令类型
        uint8_t sequence_id = 0;                      ///< 序列号
    };

    /**
     * @brief 快速追加包头
     * @param out 追加 MySQL 包头字节的输出字符串
     * @param payload_len 消息负载字节数
     * @param sequence_id 序列号
     * @return 无返回值
     */
    static void append_packet_header_fast(std::string& out, uint32_t payload_len, uint8_t sequence_id);
    /**
     * @brief 估算简单包字节数
     * @param payload_size 负载字节数
     * @return 对应的大小或数量
     */
    static size_t estimate_simple_packet_bytes(size_t payload_size) noexcept;
    /**
     * @brief 记录无法编码的命令槽位
     * @param kind 值类型
     * @param sequence_id 序列号
     * @return 无返回值
     */
    void append_invalid(MysqlCommandKind kind, uint8_t sequence_id);
    /**
     * @brief 是否包含无法编码的命令
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    [[nodiscard]] bool has_invalid_command() const noexcept;
    /**
     * @brief 快速追加简单命令
     * @param cmd 命令名称
     * @param payload 消息负载
     * @param sequence_id 序列号
     * @param kind 值类型
     * @return 无返回值
     */
    void append_simple_fast(CommandType cmd, std::string_view payload, uint8_t sequence_id, MysqlCommandKind kind);
    /**
     * @brief 按需重建命令视图
     * @return 无返回值
     */
    void rebuild_views_if_needed() const;

    std::string m_encoded;                                ///< 编码缓冲区
    std::vector<CommandMeta> m_commands;                  ///< 命令元数据列表
    mutable std::vector<MysqlCommandView> m_command_views;///< 命令视图缓存
    mutable bool m_views_dirty = true;                    ///< 视图是否需要重建

    MysqlCommandBuilder(const MysqlCommandBuilder&) = delete;
    MysqlCommandBuilder& operator=(const MysqlCommandBuilder&) = delete;
};

} // namespace galay::mysql::protocol

#endif // GALAY_MYSQL_PROTOCOL_BUILDER_H
