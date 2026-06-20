#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <galay/cpp/galay-utils/crypto/pbkdf2.hpp>

namespace {

std::string toHex(const std::vector<uint8_t>& bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

bool expectVector(const std::string& name,
                  const std::string& password,
                  const std::string& salt,
                  uint32_t iterations,
                  const std::string& expected_hex)
{
    const std::vector<uint8_t> derived = galay::utils::PBKDF2::hmacSha256(
        reinterpret_cast<const uint8_t*>(password.data()), password.size(),
        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
        iterations,
        32);

    const std::string actual = toHex(derived);
    if (actual != expected_hex) {
        std::cerr << name << " mismatch: " << actual << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    if (!expectVector("iteration 1",
                      "password",
                      "salt",
                      1,
                      "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b")) {
        return 1;
    }
    if (!expectVector("iteration 2",
                      "password",
                      "salt",
                      2,
                      "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43")) {
        return 1;
    }
    return 0;
}
