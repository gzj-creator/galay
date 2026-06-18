/**
 * @file mongo_client.h
 * @brief 同步 MongoDB 客户端
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 提供阻塞式的 MongoDB 客户端会话，包括：
 * - MongoClient 类：同步连接、命令执行和 CRUD 操作
 * - SCRAM-SHA-256 认证支持
 * - 基于 OP_MSG 协议的命令收发
 *
 * @note 不可拷贝，仅支持移动语义
 */

#ifndef GALAY_MONGO_SYNC_CLIENT_H
#define GALAY_MONGO_SYNC_CLIENT_H

#include "galay-mongo/base/mongo_config.h"
#include "galay-mongo/base/mongo_error.h"
#include "galay-mongo/base/mongo_value.h"
#include "galay-mongo/protoc/connection.h"

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

namespace galay::mongo
{

using MongoResult = std::expected<MongoReply, MongoError>;      ///< 命令执行结果类型别名
using MongoVoidResult = std::expected<void, MongoError>;        ///< 无返回值的操作结果类型别名

/**
 * @brief 同步 MongoDB 客户端会话
 * @details 提供阻塞式的连接、命令执行和 CRUD 操作。
 * 支持 SCRAM-SHA-256 认证，基于 OP_MSG 协议通信。
 *
 * @note 不可拷贝，仅支持移动语义
 */
class MongoClient
{
public:
    MongoClient();   ///< 默认构造，创建未连接的客户端
    ~MongoClient();  ///< 析构函数，自动关闭连接

    MongoClient(const MongoClient&) = delete;             ///< 禁用拷贝构造
    MongoClient& operator=(const MongoClient&) = delete;  ///< 禁用拷贝赋值

    MongoClient(MongoClient&& other) noexcept;             ///< 移动构造函数
    MongoClient& operator=(MongoClient&& other) noexcept;  ///< 移动赋值运算符

    /**
     * @brief 使用完整配置连接到 MongoDB（含认证）
     * @param config MongoDB 连接配置
     * @return 成功返回 void，失败返回 MongoError
     */
    MongoVoidResult connect(const MongoConfig& config);

    /**
     * @brief 使用地址和端口快速连接（无认证）
     * @param host 服务器地址
     * @param port 服务器端口
     * @param database 默认数据库
     * @return 成功返回 void，失败返回 MongoError
     */
    MongoVoidResult connect(const std::string& host,
                            uint16_t port,
                            const std::string& database = "admin");

    /**
     * @brief 执行任意 MongoDB 命令
     * @param database 目标数据库
     * @param command  命令文档
     * @return 响应文档或 MongoError
     */
    MongoResult command(const std::string& database, const MongoDocument& command);

    /**
     * @brief 发送 ping 命令检测连接是否存活
     * @param database 目标数据库，默认 "admin"
     * @return 响应文档或 MongoError
     */
    MongoResult ping(const std::string& database = "admin");

    /**
     * @brief 查询单条文档
     * @param database   目标数据库
     * @param collection 集合名
     * @param filter     查询条件（默认空 = 匹配全部）
     * @param projection 字段投影（默认空 = 返回全部字段）
     * @return 响应文档或 MongoError
     */
    MongoResult findOne(const std::string& database,
                        const std::string& collection,
                        const MongoDocument& filter = {},
                        const MongoDocument& projection = {});

    /**
     * @brief 插入单条文档
     * @param database   目标数据库
     * @param collection 集合名
     * @param document   待插入的文档
     * @return 响应文档或 MongoError
     */
    MongoResult insertOne(const std::string& database,
                          const std::string& collection,
                          const MongoDocument& document);

    /**
     * @brief 更新单条文档
     * @param database   目标数据库
     * @param collection 集合名
     * @param filter     匹配条件
     * @param update     更新操作（如 $set）
     * @param upsert     未匹配时是否插入，默认 false
     * @return 响应文档或 MongoError
     */
    MongoResult updateOne(const std::string& database,
                          const std::string& collection,
                          const MongoDocument& filter,
                          const MongoDocument& update,
                          bool upsert = false);

    /**
     * @brief 删除单条文档
     * @param database   目标数据库
     * @param collection 集合名
     * @param filter     匹配条件
     * @return 响应文档或 MongoError
     */
    MongoResult deleteOne(const std::string& database,
                          const std::string& collection,
                          const MongoDocument& filter);

    /**
     * @brief 关闭连接
     */
    void close();

    /**
     * @brief 判断当前是否已连接
     * @return 已连接时返回 true
     */
    bool isConnected() const { return m_connection.isConnected(); }

private:
    /**
     * @brief 执行命令并发送/接收消息
     * @param database 目标数据库
     * @param command  命令文档
     * @param check_ok 是否检查响应的 ok 字段
     * @return 响应文档或 MongoError
     */
    MongoResult runCommandRequest(const std::string& database,
                                  const MongoDocument& command,
                                  bool check_ok);

    /**
     * @brief 根据配置决定是否执行 SCRAM-SHA-256 认证
     * @param config 连接配置
     * @return 成功返回 void，失败返回 MongoError
     */
    MongoVoidResult authenticateIfNeeded(const MongoConfig& config);

    /**
     * @brief 执行 SCRAM-SHA-256 认证流程
     * @param config 包含用户名和密码的连接配置
     * @return 成功返回 void，失败返回 MongoError
     */
    MongoVoidResult authenticateScramSha256(const MongoConfig& config);

    /**
     * @brief 转义 SCRAM 用户名中的特殊字符
     * @param username 原始用户名
     * @return 转义后的用户名
     */
    static std::string escapeScramUsername(const std::string& username);

    /**
     * @brief 解析 SCRAM 交互中的 payload 键值对
     * @param payload 原始 payload 字符串
     * @return 键值对映射
     */
    static std::unordered_map<std::string, std::string>
    parseScramPayload(const std::string& payload);

    /**
     * @brief Base64 编码
     * @param bytes 原始字节
     * @return Base64 编码字符串
     */
    static std::string base64Encode(const std::vector<uint8_t>& bytes);

    /**
     * @brief Base64 解码
     * @param text Base64 编码字符串
     * @return 解码后的字节向量，或 MongoError
     */
    static std::expected<std::vector<uint8_t>, MongoError> base64Decode(const std::string& text);

    /**
     * @brief PBKDF2-HMAC-SHA256 密钥派生
     * @param password 密码
     * @param salt 盐值
     * @param iterations 迭代次数
     * @return 派生密钥，或 MongoError
     */
    static std::expected<std::vector<uint8_t>, MongoError>
    pbkdf2HmacSha256(const std::string& password,
                    const std::vector<uint8_t>& salt,
                    int iterations);

    /**
     * @brief HMAC-SHA256 计算
     * @param key 密钥
     * @param data 数据
     * @return HMAC 结果，或 MongoError
     */
    static std::expected<std::vector<uint8_t>, MongoError>
    hmacSha256(const std::vector<uint8_t>& key, const std::string& data);

    /**
     * @brief SHA-256 哈希计算
     * @param data 输入数据
     * @return 哈希结果，或 MongoError
     */
    static std::expected<std::vector<uint8_t>, MongoError>
    sha256(const std::vector<uint8_t>& data);

    /**
     * @brief 对两个字节向量执行异或操作
     * @param a 第一个字节向量
     * @param b 第二个字节向量
     * @return 异或结果
     */
    static std::vector<uint8_t> xorBytes(const std::vector<uint8_t>& a,
                                         const std::vector<uint8_t>& b);

    /**
     * @brief 生成 SCRAM 客户端随机数
     * @return Base64 编码的随机数，或 MongoError
     */
    static std::expected<std::string, MongoError> generateClientNonce();

private:
    protocol::Connection m_connection;              ///< 底层同步 TCP 连接
    MongoConfig m_config;                           ///< 连接配置
    std::string m_encoded_request_buffer;           ///< 编码请求用的临时缓冲区
    int32_t m_next_request_id = 1;                  ///< 下一个可分配的请求 ID
};

} // namespace galay::mongo

#endif // GALAY_MONGO_SYNC_CLIENT_H
