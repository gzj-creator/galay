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
std::expected<std::string, std::string> rsaOaepEncryptWithPemPublicKey(
    std::string_view payload,
    std::string_view pem_public_key);

} // namespace galay::ssl

#endif // GALAY_SSL_CRYPTO_RSA_H
