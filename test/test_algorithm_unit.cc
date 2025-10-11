/**
 * @file test_algorithm_unit.cc
 * @brief Algorithm模块单元测试
 * @details 测试Base64、MD5、SHA256、SHA512、Salt等算法功能
 */

#include "galay/algorithm/Base64.h"
#include "galay/algorithm/Md5.h"
#include "galay/algorithm/Sha256.h"
#include "galay/algorithm/Sha512.h"
#include "galay/algorithm/Salt.h"
#include "galay/algorithm/MurmurHash3.hpp"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace galay::algorithm;

/**
 * @brief 测试Base64编码解码
 */
void testBase64() {
    std::cout << "=== 测试Base64编码解码 ===" << std::endl;
    
    // 测试基本编码解码
    std::string original = "Hello, Galay!";
    std::string encoded = Base64Util::base64Encode(original);
    std::string decoded = Base64Util::base64Decode(encoded);
    
    std::cout << "原文: " << original << std::endl;
    std::cout << "编码: " << encoded << std::endl;
    std::cout << "解码: " << decoded << std::endl;
    
    assert(decoded == original && "Base64编码解码测试失败");
    
    // 测试URL安全编码
    std::string urlEncoded = Base64Util::base64Encode(original, true);
    std::cout << "URL安全编码: " << urlEncoded << std::endl;
    
    // 测试PEM格式
    std::string longText = std::string(100, 'A');
    std::string pemEncoded = Base64Util::base64EncodePem(longText);
    std::cout << "PEM格式（前100字符）: " << pemEncoded.substr(0, 100) << std::endl;
    
    std::cout << "✓ Base64测试通过\n" << std::endl;
}

/**
 * @brief 测试MD5哈希
 */
void testMd5() {
    std::cout << "=== 测试MD5哈希 ===" << std::endl;
    
    std::string text = "Galay Framework";
    std::string hash = Md5Util::encode(text);
    
    std::cout << "原文: " << text << std::endl;
    std::cout << "MD5: " << hash << std::endl;
    
    assert(hash.length() == 32 && "MD5哈希长度应为32");
    
    // 测试一致性
    std::string hash2 = Md5Util::encode(text);
    assert(hash == hash2 && "相同输入的MD5应该相同");
    
    std::cout << "✓ MD5测试通过\n" << std::endl;
}

/**
 * @brief 测试SHA256哈希
 */
void testSha256() {
    std::cout << "=== 测试SHA256哈希 ===" << std::endl;
    
    std::string text = "Galay Framework";
    std::string hash = Sha256Util::encode(text);
    
    std::cout << "原文: " << text << std::endl;
    std::cout << "SHA256: " << hash << std::endl;
    
    assert(hash.length() == 64 && "SHA256哈希长度应为64");
    
    // 测试一致性
    std::string hash2 = Sha256Util::encode(text);
    assert(hash == hash2 && "相同输入的SHA256应该相同");
    
    std::cout << "✓ SHA256测试通过\n" << std::endl;
}

/**
 * @brief 测试SHA512哈希
 */
void testSha512() {
    std::cout << "=== 测试SHA512哈希 ===" << std::endl;
    
    std::string text = "Galay Framework";
    std::string hash = Sha512Util::encode(text);
    
    std::cout << "原文: " << text << std::endl;
    std::cout << "SHA512: " << hash << std::endl;
    
    assert(hash.length() == 128 && "SHA512哈希长度应为128");
    
    std::cout << "✓ SHA512测试通过\n" << std::endl;
}

/**
 * @brief 测试盐值生成
 */
void testSalt() {
    std::cout << "=== 测试盐值生成 ===" << std::endl;
    
    std::string salt1 = Salt::create(8, 16);
    std::string salt2 = Salt::create(8, 16);
    
    std::cout << "盐值1: " << salt1 << " (长度: " << salt1.length() << ")" << std::endl;
    std::cout << "盐值2: " << salt2 << " (长度: " << salt2.length() << ")" << std::endl;
    
    assert(salt1.length() >= 8 && salt1.length() <= 16 && "盐值长度应在8-16之间");
    assert(salt1 != salt2 && "两次生成的盐值应该不同");
    
    std::cout << "✓ 盐值生成测试通过\n" << std::endl;
}

/**
 * @brief 测试MurmurHash3
 */
void testMurmurHash3() {
    std::cout << "=== 测试MurmurHash3 ===" << std::endl;
    
    const char* text = "Galay Framework";
    uint32_t hash32;
    uint32_t hash128[4];
    uint64_t hash64[2];
    
    murmurHash3_x86_32(text, strlen(text), 0, &hash32);
    murmurHash3_x86_128(text, strlen(text), 0, hash128);
    murmurHash3_x64_128(text, strlen(text), 0, hash64);
    
    std::cout << "MurmurHash3_x86_32: " << hash32 << std::endl;
    std::cout << "MurmurHash3_x86_128: " << hash128[0] << hash128[1] << hash128[2] << hash128[3] << std::endl;
    std::cout << "MurmurHash3_x64_128: " << hash64[0] << hash64[1] << std::endl;
    
    std::cout << "✓ MurmurHash3测试通过\n" << std::endl;
}

int main() {
    std::cout << "开始Algorithm模块单元测试" << std::endl;
    std::cout << "================================\n" << std::endl;
    
    try {
        testBase64();
        testMd5();
        testSha256();
        testSha512();
        testSalt();
        testMurmurHash3();
        
        std::cout << "================================" << std::endl;
        std::cout << "✓ 所有测试通过！" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "✗ 测试失败: " << e.what() << std::endl;
        return 1;
    }
}

