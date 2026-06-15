/**
 * @file mysql_auth.h
 * @brief MySQL认证插件实现
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 提供MySQL协议中常用的认证算法实现，包括mysql_native_password、
 *          caching_sha2_password的快速认证和全量认证（RSA公钥加密）。
 */

#ifndef GALAY_MYSQL_AUTH_H
#define GALAY_MYSQL_AUTH_H

#include <expected>
#include <string>
#include <string_view>

namespace galay::mysql::protocol
{

/**
 * @brief MySQL认证插件
 */
class AuthPlugin
{
public:
    /**
     * @brief mysql_native_password 认证
     * @details SHA1(password) XOR SHA1(salt + SHA1(SHA1(password)))
     * @param password 明文密码
     * @param salt 服务器发送的salt（20字节）
     * @return 认证数据（20字节）
     */
    static std::string nativePasswordAuth(const std::string& password, const std::string& salt);

    /**
     * @brief caching_sha2_password 认证
     * @details XOR(SHA256(password), SHA256(SHA256(SHA256(password)) + salt))
     * @param password 明文密码
     * @param salt 服务器发送的salt（20字节）
     * @return 认证数据（32字节）
     */
    static std::string cachingSha2Auth(const std::string& password, const std::string& salt);

    /**
     * @brief caching_sha2_password 全量认证（RSA 公钥加密）
     * @details password + '\0' 与 handshake salt 异或后，用服务端 RSA 公钥做 OAEP 加密
     * @param password 明文密码
     * @param salt 服务器发送的 salt
     * @param pem_public_key 服务端返回的 PEM 公钥
     * @return 加密后的认证数据
     */
    static std::expected<std::string, std::string> cachingSha2FullAuth(const std::string& password,
                                                                       const std::string& salt,
                                                                       std::string_view pem_public_key);

    /**
     * @brief SHA1哈希
     */
    static std::string sha1(const std::string& data);

    /**
     * @brief SHA256哈希
     */
    static std::string sha256(const std::string& data);

    /**
     * @brief XOR两个等长字符串
     */
    static std::string xorStrings(const std::string& a, const std::string& b);
};

} // namespace galay::mysql::protocol

#endif // GALAY_MYSQL_AUTH_H
