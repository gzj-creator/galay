#include <iostream>
#include <cassert>
#include <iomanip>
#include "galay-mysql/protoc/mysql_auth.h"

using namespace galay::mysql::protocol;

void printHex(const std::string& data, const std::string& label)
{
    std::cout << "  " << label << " (" << data.size() << " bytes): ";
    for (unsigned char c : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    }
    std::cout << std::dec << std::endl;
}

void test_sha1()
{
    std::cout << "Testing SHA1..." << std::endl;
    auto hash = AuthPlugin::sha1("hello");
    assert(hash.size() == 20);
    printHex(hash, "SHA1('hello')");
    std::cout << "  PASSED" << std::endl;
}

void test_sha256()
{
    std::cout << "Testing SHA256..." << std::endl;
    auto hash = AuthPlugin::sha256("hello");
    assert(hash.size() == 32);
    printHex(hash, "SHA256('hello')");
    std::cout << "  PASSED" << std::endl;
}

void test_xor_strings()
{
    std::cout << "Testing XOR strings..." << std::endl;
    std::string a = "\x01\x02\x03\x04";
    std::string b = "\x05\x06\x07\x08";
    auto result = AuthPlugin::xorStrings(a, b);
    assert(result.size() == 4);
    assert(result[0] == '\x04');
    assert(result[1] == '\x04');
    assert(result[2] == '\x04');
    assert(result[3] == '\x0c');
    std::cout << "  PASSED" << std::endl;
}

void test_native_password_auth()
{
    std::cout << "Testing mysql_native_password auth..." << std::endl;
    std::string salt = "12345678901234567890";
    auto result = AuthPlugin::nativePasswordAuth("password", salt);
    assert(result.size() == 20);
    printHex(result, "native_password_auth");

    // 空密码应返回空字符串
    auto empty = AuthPlugin::nativePasswordAuth("", salt);
    assert(empty.empty());

    std::cout << "  PASSED" << std::endl;
}

void test_caching_sha2_auth()
{
    std::cout << "Testing caching_sha2_password auth..." << std::endl;
    std::string salt = "12345678901234567890";
    auto result = AuthPlugin::cachingSha2Auth("password", salt);
    assert(result.size() == 32);
    printHex(result, "caching_sha2_auth");

    auto empty = AuthPlugin::cachingSha2Auth("", salt);
    assert(empty.empty());

    std::cout << "  PASSED" << std::endl;
}

void test_auth_response_for_plugin()
{
    std::cout << "Testing auth response plugin dispatch..." << std::endl;

    const std::string password = "password";
    const std::string salt = "12345678901234567890";

    auto native = AuthPlugin::authResponseForPlugin("mysql_native_password", password, salt);
    assert(native.has_value());
    assert(native.value() == AuthPlugin::nativePasswordAuth(password, salt));
    assert(native->size() == 20);

    auto caching = AuthPlugin::authResponseForPlugin("caching_sha2_password", password, salt);
    assert(caching.has_value());
    assert(caching.value() == AuthPlugin::cachingSha2Auth(password, salt));
    assert(caching->size() == 32);

    auto empty_native = AuthPlugin::authResponseForPlugin("mysql_native_password", "", salt);
    assert(empty_native.has_value());
    assert(empty_native->empty());

    auto unsupported = AuthPlugin::authResponseForPlugin("sha256_password", password, salt);
    assert(!unsupported.has_value());
    assert(unsupported.error() == "Unsupported auth plugin: sha256_password");

    auto empty_plugin = AuthPlugin::authResponseForPlugin("", password, salt);
    assert(!empty_plugin.has_value());
    assert(empty_plugin.error() == "Unsupported auth plugin: ");

    std::cout << "  PASSED" << std::endl;
}

void test_caching_sha2_full_auth()
{
    std::cout << "Testing caching_sha2_password full auth..." << std::endl;

    const std::string public_key_pem =
        "-----BEGIN PUBLIC KEY-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAwPEGdRCRpZDptJqhD1Zk\n"
        "U6Zz5XhBqL3DGi18XgpjmgJBKsY5Zli75fD9snqkb738i/DLBN1+6UGBeYxdOpT8\n"
        "RE4/8kbTAAilMjp20/B1LI8rMf2g/R4wfVncBw+DaU7LNBtOv39I133DnyyjWX2l\n"
        "BykLLTzHCNrQwPHgx7x73Qo65x9GVkk1QVfAdMH2LTohc4KRHyprsJ5UFzPi2R0j\n"
        "+U8mDTovuptlslCTQg721EOE2iDjfvPRe9qo6t+hb/OtAKa1Hzks7CmXo+8GbNji\n"
        "kVWUbSH3+ubRm0Y4ABMRYrTcMip2KlXWW0Qlgiuiiug5r7M94W0fSFoUh1qDv+x9\n"
        "0QIDAQAB\n"
        "-----END PUBLIC KEY-----\n";

    const std::string password = "GalayPass_123!";
    const std::string salt = "12345678901234567890";

    auto encrypted = AuthPlugin::cachingSha2FullAuth(password, salt, public_key_pem);
#ifdef GALAY_SSL_FEATURE_ENABLED
    assert(encrypted.has_value());
    assert(encrypted->size() == 256);
#else
    assert(!encrypted.has_value());
#endif
    std::cout << "  PASSED" << std::endl;
}

void test_caching_sha2_full_auth_rejects_bad_key()
{
    std::cout << "Testing caching_sha2_password full auth bad key handling..." << std::endl;

    const std::string password = "GalayPass_123!";
    const std::string salt = "12345678901234567890";

    auto empty_key = AuthPlugin::cachingSha2FullAuth(password, salt, "");
    assert(!empty_key.has_value());

    auto nul_only_key = AuthPlugin::cachingSha2FullAuth(password, salt, std::string_view("\0", 1));
    assert(!nul_only_key.has_value());

#ifdef GALAY_SSL_FEATURE_ENABLED
    assert(empty_key.error() == "empty RSA public key");
    assert(nul_only_key.error() == "empty RSA public key");

    auto invalid_key = AuthPlugin::cachingSha2FullAuth(password, salt, "not a pem public key");
    assert(!invalid_key.has_value());
#else
    assert(empty_key.error() == "caching_sha2_password RSA authentication requires GALAY_BUILD_SSL=ON");
    assert(nul_only_key.error() == "caching_sha2_password RSA authentication requires GALAY_BUILD_SSL=ON");
#endif

    std::cout << "  PASSED" << std::endl;
}

int main()
{
    std::cout << "=== T2: MySQL Auth Tests ===" << std::endl;

    test_sha1();
    test_sha256();
    test_xor_strings();
    test_native_password_auth();
    test_caching_sha2_auth();
    test_auth_response_for_plugin();
    test_caching_sha2_full_auth();
    test_caching_sha2_full_auth_rejects_bad_key();

    std::cout << "\nAll auth tests PASSED!" << std::endl;
    return 0;
}
