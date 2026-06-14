#include "mysql_auth.h"
#include <utils/crypto/hmac.hpp>
#include <utils/crypto/sha1.hpp>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "ssl/crypto/rsa.h"
#endif

#include <cstring>
#include <algorithm>

namespace galay::mysql::protocol
{

std::string AuthPlugin::sha1(const std::string& data)
{
    const auto digest = galay::utils::SHA1::hash(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string AuthPlugin::sha256(const std::string& data)
{
    const auto digest = galay::utils::SHA256::hash(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string AuthPlugin::xorStrings(const std::string& a, const std::string& b)
{
    size_t min_len = std::min(a.size(), b.size());
    std::string result(min_len, '\0');
    for (size_t i = 0; i < min_len; ++i) {
        result[i] = a[i] ^ b[i];
    }
    return result;
}

std::string AuthPlugin::nativePasswordAuth(const std::string& password, const std::string& salt)
{
    if (password.empty()) {
        return "";
    }

    // SHA1(password)
    std::string hash1 = sha1(password);
    // SHA1(SHA1(password))
    std::string hash2 = sha1(hash1);
    // SHA1(salt + SHA1(SHA1(password)))
    std::string combined = salt + hash2;
    std::string hash3 = sha1(combined);
    // SHA1(password) XOR SHA1(salt + SHA1(SHA1(password)))
    return xorStrings(hash1, hash3);
}

std::string AuthPlugin::cachingSha2Auth(const std::string& password, const std::string& salt)
{
    if (password.empty()) {
        return "";
    }

    // SHA256(password)
    std::string hash1 = sha256(password);
    // SHA256(SHA256(password))
    std::string hash2 = sha256(hash1);
    // SHA256(SHA256(SHA256(password)) + salt)
    std::string combined = hash2 + salt;
    std::string hash3 = sha256(combined);
    // XOR(SHA256(password), SHA256(SHA256(SHA256(password)) + salt))
    return xorStrings(hash1, hash3);
}

std::expected<std::string, std::string> AuthPlugin::cachingSha2FullAuth(const std::string& password,
                                                                        const std::string& salt,
                                                                        std::string_view pem_public_key)
{
#ifndef GALAY_SSL_FEATURE_ENABLED
    (void)password;
    (void)salt;
    (void)pem_public_key;
    return std::unexpected("caching_sha2_password RSA authentication requires GALAY_BUILD_SSL=ON");
#else
    std::string public_key(pem_public_key);
    if (!public_key.empty() && public_key.back() == '\0') {
        public_key.pop_back();
    }
    if (public_key.empty()) {
        return std::unexpected("empty RSA public key");
    }

    std::string payload = password;
    payload.push_back('\0');

    if (!salt.empty()) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] ^= salt[i % salt.size()];
        }
    }

    auto encrypted = galay::ssl::rsaOaepEncryptWithPemPublicKey(payload, public_key);
    if (!encrypted) {
        return std::unexpected(encrypted.error());
    }
    return *encrypted;
#endif
}

} // namespace galay::mysql::protocol
