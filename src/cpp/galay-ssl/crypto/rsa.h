/**
 * @file rsa.h
 * @brief OpenSSL-backed RSA helpers exposed through galay::ssl
 */

#ifndef GALAY_SSL_CRYPTO_RSA_H
#define GALAY_SSL_CRYPTO_RSA_H

#include <expected>
#include <string>
#include <string_view>

namespace galay::ssl
{

/**
 * @brief Encrypt data with a PEM public key using RSA OAEP padding.
 *
 * @param payload Plaintext bytes to encrypt.
 * @param pem_public_key PEM-encoded public key.
 * @return Ciphertext bytes, or an error message describing the OpenSSL failure.
 */
std::expected<std::string, std::string> rsa_oaep_encrypt_with_pem_public_key(
    std::string_view payload,
    std::string_view pem_public_key);

/**
 * @brief Encrypt data with a PEM public key using RSA OAEP with SHA-1.
 *
 * This variant is required by wire protocols such as MySQL
 * caching_sha2_password full authentication.
 * @param payload 消息负载
 * @param pem_public_key PEM 格式公钥
 * @return 成功时返回 std::string，失败时返回 std::string 错误
 */
std::expected<std::string, std::string> rsa_oaep_sha1_encrypt_with_pem_public_key(
    std::string_view payload,
    std::string_view pem_public_key);

} // namespace galay::ssl

#endif // GALAY_SSL_CRYPTO_RSA_H
