#include "rsa.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace galay::ssl
{

namespace {

std::string getOpenSslError()
{
    const unsigned long error = ERR_get_error();
    if (error == 0) {
        return "unknown OpenSSL error";
    }
    char buffer[256];
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return std::string(buffer);
}

} // namespace

std::expected<std::string, std::string> rsaOaepEncryptWithPemPublicKey(
    std::string_view payload,
    std::string_view pem_public_key)
{
    if (pem_public_key.empty()) {
        return std::unexpected("empty RSA public key");
    }

    BIO* bio = BIO_new_mem_buf(pem_public_key.data(), static_cast<int>(pem_public_key.size()));
    if (!bio) {
        return std::unexpected("BIO_new_mem_buf failed: " + getOpenSslError());
    }

    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        return std::unexpected("PEM_read_bio_PUBKEY failed: " + getOpenSslError());
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_CTX_new failed: " + getOpenSslError());
    }

    if (EVP_PKEY_encrypt_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_encrypt_init failed: " + getOpenSslError());
    }

    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_CTX_set_rsa_padding failed: " + getOpenSslError());
    }

    size_t encrypted_size = 0;
    if (EVP_PKEY_encrypt(ctx,
                         nullptr,
                         &encrypted_size,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_encrypt(size) failed: " + getOpenSslError());
    }

    std::string encrypted(encrypted_size, '\0');
    if (EVP_PKEY_encrypt(ctx,
                         reinterpret_cast<unsigned char*>(encrypted.data()),
                         &encrypted_size,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_encrypt(data) failed: " + getOpenSslError());
    }

    encrypted.resize(encrypted_size);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return encrypted;
}

} // namespace galay::ssl
