/**
 * @file mysql_client.h
 * @brief 同步MySQL客户端
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 基于readv/writev和环形缓冲区的同步MySQL客户端实现，
 *          提供连接、查询、预处理语句、事务、批量操作等功能。
 */

#ifndef GALAY_MYSQL_SYNC_CLIENT_H
#define GALAY_MYSQL_SYNC_CLIENT_H

#include "galay-mysql/base/mysql_config.h"
#include "galay-mysql/base/mysql_error.h"
#include "galay-mysql/base/mysql_value.h"
#include "galay-mysql/protoc/builder.h"
#include "galay-mysql/protoc/mysql_auth.h"
#include "galay-mysql/protoc/mysql_protocol.h"

#include <galay-utils/cache/ring_buffer.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <utility>
#include <vector>

namespace galay::mysql
{

using MysqlResult = std::expected<MysqlResultSet, MysqlError>;                             ///< 查询结果类型
using MysqlVoidResult = std::expected<void, MysqlError>;                                   ///< 无返回值操作结果类型
using MysqlBatchResult = std::expected<std::vector<MysqlResultSet>, MysqlError>;            ///< 批量操作结果类型

/**
 * @brief 同步MySQL客户端（readv/writev + ring buffer）
 * @details 提供同步阻塞方式的MySQL客户端操作，适用于非异步场景。
 *          内部使用环形缓冲区和分散/聚集IO优化网络读写性能。
 */
class MysqlClient
{
public:
    MysqlClient();    ///< 构造同步MySQL客户端
    ~MysqlClient();   ///< 析构并关闭连接

    MysqlClient(const MysqlClient&) = delete;              ///< 禁止拷贝构造
    MysqlClient& operator=(const MysqlClient&) = delete;   ///< 禁止拷贝赋值
    MysqlClient(MysqlClient&& other) noexcept;             ///< 移动构造
    MysqlClient& operator=(MysqlClient&& other) noexcept;  ///< 移动赋值

    // ======================== 连接 ========================

    /**
     * @brief 连接到MySQL服务器
     * @param config MySQL连接配置
     * @return 成功或错误
     */
    MysqlVoidResult connect(const MysqlConfig& config);

    /**
     * @brief 连接到MySQL服务器（参数形式）
     * @param host 服务器地址
     * @param port 服务器端口
     * @param user 用户名
     * @param password 密码
     * @param database 默认数据库
     * @return 成功或错误
     */
    MysqlVoidResult connect(const std::string& host, uint16_t port,
                            const std::string& user, const std::string& password,
                            const std::string& database = "");

    // ======================== 查询 ========================

    /**
     * @brief 执行SQL查询
     * @param sql SQL语句
     * @return 查询结果集或错误
     */
    MysqlResult query(const std::string& sql);

    /**
     * @brief 批量执行编码后的命令
     * @param commands 编码后的命令视图数组
     * @return 批量结果集或错误
     */
    MysqlBatchResult batch(std::span<const protocol::MysqlCommandView> commands);

    /**
     * @brief Pipeline执行多条SQL语句
     * @param sqls SQL语句数组
     * @return 批量结果集或错误
     */
    MysqlBatchResult pipeline(std::span<const std::string_view> sqls);

    // ======================== 预处理语句 ========================

    /**
     * @brief 预处理语句结果
     */
    struct PrepareResult {
        uint32_t statement_id;  ///< 语句ID
        uint16_t num_columns;   ///< 列数
        uint16_t num_params;    ///< 参数数量
    };

    /**
     * @brief 准备预处理语句
     * @param sql 预处理SQL语句
     * @return 预处理结果或错误
     */
    std::expected<PrepareResult, MysqlError> prepare(const std::string& sql);

    /**
     * @brief 执行预处理语句
     * @param stmt_id 语句ID（由prepare返回）
     * @param params 参数值列表
     * @param param_types 参数类型列表
     * @return 查询结果集或错误
     */
    MysqlResult stmtExecute(uint32_t stmt_id,
                            const std::vector<std::optional<std::string>>& params,
                            const std::vector<uint8_t>& param_types = {});

    /**
     * @brief 关闭预处理语句
     * @param stmt_id 语句ID
     * @return 成功或错误
     */
    MysqlVoidResult stmtClose(uint32_t stmt_id);

    // ======================== 事务 ========================

    MysqlVoidResult beginTransaction();  ///< 开启事务
    MysqlVoidResult commit();             ///< 提交事务
    MysqlVoidResult rollback();           ///< 回滚事务

    // ======================== 工具 ========================

    MysqlVoidResult ping();                                ///< 发送心跳检测
    MysqlVoidResult useDatabase(const std::string& database); ///< 切换数据库

    // ======================== 连接管理 ========================

    void close();                                           ///< 关闭连接
    bool isConnected() const { return m_connected; }        ///< 检查是否已连接

private:
    using Packet = std::pair<uint8_t, std::string>; ///< 包类型：序列号 + payload

    static constexpr size_t kRecvBufferCapacity = 256 * 1024; ///< 接收缓冲区容量（256KB）

    MysqlVoidResult connectSocket(const std::string& host, uint16_t port, uint32_t timeout_ms); ///< 创建TCP连接
    void closeSocket() noexcept; ///< 关闭套接字

    MysqlVoidResult sendAll(std::string_view data); ///< 发送全部数据
    MysqlVoidResult sendAllv(std::span<const struct iovec> iovecs); ///< 通过iovec发送全部数据

    MysqlVoidResult recvIntoRingBuffer(); ///< 从套接字读取数据到环形缓冲区
    std::expected<std::optional<Packet>, MysqlError> tryExtractPacket(); ///< 尝试从环形缓冲区提取包
    std::expected<Packet, MysqlError> recvPacket(); ///< 接收一个完整的MySQL包

    MysqlResult receiveResultSet(); ///< 接收完整结果集
    MysqlVoidResult runSimpleStatement(const std::string& sql); ///< 执行简单SQL语句

    int m_socket_fd;                             ///< 套接字文件描述符
    bool m_connected;                            ///< 是否已连接
    galay::utils::RingBuffer m_recv_ring_buffer; ///< 接收环形缓冲区
    std::string m_parse_scratch;                 ///< 解析临时缓冲区

    protocol::MysqlParser m_parser;              ///< 协议解析器
    protocol::MysqlEncoder m_encoder;            ///< 协议编码器
    uint32_t m_server_capabilities = 0;          ///< 服务器能力标志
};

} // namespace galay::mysql

#endif // GALAY_MYSQL_SYNC_CLIENT_H
