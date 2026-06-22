/**
 * @file t14_security_lifecycle.cc
 * @brief 锁定 OpenSSL 初始化、hostname 校验、RSA OAEP 和 BIO 生命周期边界。
 */

#include <galay/cpp/galay-ssl/crypto/rsa.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-ssl/ssl/ssl_engine.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

template <typename T, void (*Deleter)(T*)>
using OpenSslPtr = std::unique_ptr<T, decltype(Deleter)>;

void freeBio(BIO* bio)
{
    BIO_free(bio);
}

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path projectRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

OpenSslPtr<EVP_PKEY, EVP_PKEY_free> generateRsaKey()
{
    OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    assert(ctx);
    assert(EVP_PKEY_keygen_init(ctx.get()) > 0);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) > 0);

    EVP_PKEY* raw = nullptr;
    assert(EVP_PKEY_keygen(ctx.get(), &raw) > 0);
    return OpenSslPtr<EVP_PKEY, EVP_PKEY_free>(raw, EVP_PKEY_free);
}

std::string publicKeyPem(EVP_PKEY* key)
{
    OpenSslPtr<BIO, freeBio> bio(BIO_new(BIO_s_mem()), freeBio);
    assert(bio);
    assert(PEM_write_bio_PUBKEY(bio.get(), key) == 1);

    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(bio.get(), &memory);
    assert(memory != nullptr);
    return {memory->data, memory->length};
}

bool decryptsWithSha256Oaep(EVP_PKEY* key, std::string_view ciphertext)
{
    OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> ctx(EVP_PKEY_CTX_new(key, nullptr), EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_decrypt_init(ctx.get()) <= 0) {
        return false;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) <= 0) {
        return false;
    }
    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) <= 0) {
        return false;
    }
    if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), EVP_sha256()) <= 0) {
        return false;
    }

    std::size_t plainSize = 0;
    const auto* encrypted = reinterpret_cast<const unsigned char*>(ciphertext.data());
    if (EVP_PKEY_decrypt(ctx.get(), nullptr, &plainSize, encrypted, ciphertext.size()) <= 0) {
        return false;
    }

    std::string plaintext(plainSize, '\0');
    if (EVP_PKEY_decrypt(ctx.get(),
                         reinterpret_cast<unsigned char*>(plaintext.data()),
                         &plainSize,
                         encrypted,
                         ciphertext.size()) <= 0) {
        return false;
    }
    plaintext.resize(plainSize);
    return plaintext == "galay-rsa-oaep-sha256";
}

void openSslGlobalInitIsCallOnce()
{
    const auto source = readAll(projectRoot() / "src/cpp/galay-ssl/ssl/ssl_context.cc");
    assert(source.find("std::once_flag") != std::string::npos);
    assert(source.find("std::call_once") != std::string::npos);
}

void hostnameReturnValueIsChecked()
{
    const auto source = readAll(projectRoot() / "src/cpp/galay-ssl/ssl/ssl_engine.cc");
    assert(source.find("SSL_set1_host(m_ssl, hostname.c_str()) != 1") != std::string::npos);
}

void memoryBioInitializationIsIdempotent()
{
    galay::ssl::SslContext context(galay::ssl::SslMethod::TLS_Client);
    galay::ssl::SslEngine engine(&context);

    assert(engine.initMemoryBIO().has_value());
    BIO* firstReadBio = SSL_get_rbio(engine.native());
    BIO* firstWriteBio = SSL_get_wbio(engine.native());
    assert(firstReadBio != nullptr);
    assert(firstWriteBio != nullptr);

    assert(engine.initMemoryBIO().has_value());
    assert(SSL_get_rbio(engine.native()) == firstReadBio);
    assert(SSL_get_wbio(engine.native()) == firstWriteBio);
}

void rsaOaepUsesSha256ForOaepAndMgf1()
{
    auto key = generateRsaKey();
    const auto publicPem = publicKeyPem(key.get());
    auto encrypted = galay::ssl::rsaOaepEncryptWithPemPublicKey("galay-rsa-oaep-sha256", publicPem);
    assert(encrypted.has_value());
    assert(decryptsWithSha256Oaep(key.get(), *encrypted));
}

} // namespace

int main()
{
    openSslGlobalInitIsCallOnce();
    hostnameReturnValueIsChecked();
    memoryBioInitializationIsIdempotent();
    rsaOaepUsesSha256ForOaepAndMgf1();
    std::cout << "T14-SslSecurityLifecycle PASS\n";
    return 0;
}
