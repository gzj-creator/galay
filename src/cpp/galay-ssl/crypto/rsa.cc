#include "rsa.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

namespace galay::ssl
{

namespace {

std::string get_open_ssl_error()
{
    const unsigned long error = ERR_get_error();
    if (error == 0) {
        return "unknown OpenSSL error";
    }
    char buffer[256];
    ERR_error_string_n(error, buffer, sizeof(buffer));
    return std::string(buffer);
}

std::expected<std::string, std::string> rsa_oaep_encrypt_with_pem_public_key_and_digest(
    std::string_view payload,
    std::string_view pem_public_key,
    const EVP_MD* digest)
{
    if (pem_public_key.empty()) {
        return std::unexpected("empty RSA public key");
    }

    BIO* bio = BIO_new_mem_buf(pem_public_key.data(), static_cast<int>(pem_public_key.size()));
    if (!bio) {
        return std::unexpected("BIO_new_mem_buf failed: " + get_open_ssl_error());
    }

    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        return std::unexpected("PEM_read_bio_PUBKEY failed: " + get_open_ssl_error());
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_CTX_new failed: " + get_open_ssl_error());
    }

    if (EVP_PKEY_encrypt_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_encrypt_init failed: " + get_open_ssl_error());
    }

    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_CTX_set_rsa_padding failed: " + get_open_ssl_error());
    }
    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, digest) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_CTX_set_rsa_oaep_md failed: " + get_open_ssl_error());
    }
    if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, digest) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_CTX_set_rsa_mgf1_md failed: " + get_open_ssl_error());
    }

    size_t encrypted_size = 0;
    if (EVP_PKEY_encrypt(ctx,
                         nullptr,
                         &encrypted_size,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_encrypt(size) failed: " + get_open_ssl_error());
    }

    std::string encrypted(encrypted_size, '\0');
    if (EVP_PKEY_encrypt(ctx,
                         reinterpret_cast<unsigned char*>(encrypted.data()),
                         &encrypted_size,
                         reinterpret_cast<const unsigned char*>(payload.data()),
                         payload.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected("EVP_PKEY_encrypt(data) failed: " + get_open_ssl_error());
    }

    encrypted.resize(encrypted_size);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return encrypted;
}

} // namespace

std::expected<std::string, std::string> rsa_oaep_encrypt_with_pem_public_key(
    std::string_view payload,
    std::string_view pem_public_key)
{
    return rsa_oaep_encrypt_with_pem_public_key_and_digest(payload, pem_public_key, EVP_sha256());
}

std::expected<std::string, std::string> rsa_oaep_sha1_encrypt_with_pem_public_key(
    std::string_view payload,
    std::string_view pem_public_key)
{
    return rsa_oaep_encrypt_with_pem_public_key_and_digest(payload, pem_public_key, EVP_sha1());
}

} // namespace galay::ssl
