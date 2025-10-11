/**
 * @file test_buffer_unit.cc
 * @brief Buffer和Bytes模块单元测试
 * @details 测试Buffer和Bytes的各种操作
 */

#include "galay/common/Buffer.h"
#include "galay/kernel/async/Bytes.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace galay;

/**
 * @brief 测试Buffer基本操作
 */
void testBufferBasic() {
    std::cout << "=== 测试Buffer基本操作 ===" << std::endl;
    
    // 测试默认构造
    Buffer buf1;
    assert(buf1.length() == 0 && "默认构造的Buffer长度应为0");
    
    // 测试从字符串构造
    std::string str = "Hello, Galay!";
    Buffer buf2(str);
    assert(buf2.length() == str.length() && "Buffer长度应与字符串相同");
    assert(buf2.toString() == str && "Buffer内容应与字符串相同");
    
    // 测试从数据指针构造
    const char* data = "Test Data";
    Buffer buf3(data, strlen(data));
    assert(buf3.length() == strlen(data) && "Buffer长度应正确");
    
    std::cout << "✓ Buffer基本操作测试通过\n" << std::endl;
}

/**
 * @brief 测试Buffer容量管理
 */
void testBufferCapacity() {
    std::cout << "=== 测试Buffer容量管理 ===" << std::endl;
    
    Buffer buf(100);
    assert(buf.capacity() >= 100 && "Buffer容量应至少为100");
    
    // 测试resize
    buf.resize(200);
    assert(buf.capacity() >= 200 && "Buffer容量应至少为200");
    
    // 测试clear
    Buffer buf2("Test");
    buf2.clear();
    assert(buf2.length() == 0 && "清空后长度应为0");
    
    std::cout << "✓ Buffer容量管理测试通过\n" << std::endl;
}

/**
 * @brief 测试Buffer转换
 */
void testBufferConversion() {
    std::cout << "=== 测试Buffer转换 ===" << std::endl;
    
    std::string original = "Conversion Test";
    Buffer buf(original);
    
    // 测试toString
    std::string str = buf.toString();
    assert(str == original && "toString应返回原始字符串");
    
    // 测试toStringView
    std::string_view sv = buf.toStringView();
    assert(sv == original && "toStringView应返回正确的视图");
    
    std::cout << "✓ Buffer转换测试通过\n" << std::endl;
}

/**
 * @brief 测试Bytes基本操作
 */
void testBytesBasic() {
    std::cout << "=== 测试Bytes基本操作 ===" << std::endl;
    
    // 测试默认构造
    Bytes bytes1;
    assert(bytes1.empty() && "默认构造的Bytes应为空");
    
    // 测试从字符串构造
    std::string str = "Hello, Bytes!";
    Bytes bytes2(str);
    assert(bytes2.size() == str.length() && "Bytes大小应与字符串相同");
    
    // 测试从C字符串构造
    const char* cstr = "C String";
    Bytes bytes3(cstr);
    assert(bytes3.size() == strlen(cstr) && "Bytes大小应正确");
    
    // 测试移动构造
    Bytes bytes4(std::move(bytes2));
    assert(bytes4.size() == str.length() && "移动构造后大小应正确");
    
    std::cout << "✓ Bytes基本操作测试通过\n" << std::endl;
}

/**
 * @brief 测试Bytes数据访问
 */
void testBytesAccess() {
    std::cout << "=== 测试Bytes数据访问 ===" << std::endl;
    
    std::string str = "Test Data";
    Bytes bytes(str);
    
    // 测试data()
    const uint8_t* data = bytes.data();
    assert(data != nullptr && "data()应返回非空指针");
    
    // 测试c_str()
    const char* cstr = bytes.c_str();
    assert(strcmp(cstr, str.c_str()) == 0 && "c_str()应返回正确的C字符串");
    
    // 测试size()和empty()
    assert(!bytes.empty() && "非空Bytes不应为empty");
    assert(bytes.size() == str.length() && "size()应返回正确大小");
    
    std::cout << "✓ Bytes数据访问测试通过\n" << std::endl;
}

/**
 * @brief 测试Bytes转换
 */
void testBytesConversion() {
    std::cout << "=== 测试Bytes转换 ===" << std::endl;
    
    std::string original = "Conversion Test";
    Bytes bytes(original);
    
    // 测试toString
    std::string str = bytes.toString();
    assert(str == original && "toString应返回原始字符串");
    
    // 测试toStringView
    std::string_view sv = bytes.toStringView();
    assert(sv == original && "toStringView应返回正确的视图");
    
    // 测试fromString
    std::string str2 = "From String";
    Bytes bytes2 = Bytes::fromString(str2);
    assert(bytes2.toString() == str2 && "fromString应正确创建Bytes");
    
    std::cout << "✓ Bytes转换测试通过\n" << std::endl;
}

/**
 * @brief 测试Bytes比较
 */
void testBytesComparison() {
    std::cout << "=== 测试Bytes比较 ===" << std::endl;
    
    Bytes bytes1("Hello");
    Bytes bytes2("Hello");
    Bytes bytes3("World");
    
    assert(bytes1 == bytes2 && "相同内容的Bytes应该相等");
    assert(bytes1 != bytes3 && "不同内容的Bytes应该不等");
    
    std::cout << "✓ Bytes比较测试通过\n" << std::endl;
}

int main() {
    std::cout << "开始Buffer和Bytes模块单元测试" << std::endl;
    std::cout << "================================\n" << std::endl;
    
    try {
        testBufferBasic();
        testBufferCapacity();
        testBufferConversion();
        testBytesBasic();
        testBytesAccess();
        testBytesConversion();
        testBytesComparison();
        
        std::cout << "================================" << std::endl;
        std::cout << "✓ 所有测试通过！" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "✗ 测试失败: " << e.what() << std::endl;
        return 1;
    }
}

