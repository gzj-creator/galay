/**
 * @file test_chunk.cc
 * @brief HTTP Chunk 类单元测试
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <galay/cpp/galay-http/protoc/http_chunk.h>

using namespace galay::http;

void test_chunk_toChunk() {
    std::cout << "Testing Chunk::toChunk()..." << std::endl;

    // 测试1: 普通chunk
    std::string data1 = "Hello";
    std::string chunk1 = Chunk::toChunk(data1, false);
    std::string expected1 = "5\r\nHello\r\n";
    assert(chunk1 == expected1);
    std::cout << "  ✓ Normal chunk: " << chunk1.size() << " bytes" << std::endl;

    // 测试2: 最后一个chunk
    std::string empty_data = "";
    std::string chunk2 = Chunk::toChunk(empty_data, true);
    std::string expected2 = "0\r\n\r\n";
    assert(chunk2 == expected2);
    std::cout << "  ✓ Last chunk: " << chunk2.size() << " bytes" << std::endl;

    // 测试3: 从buffer创建
    const char* buffer = "World!";
    std::string chunk3 = Chunk::toChunk(buffer, 6, false);
    std::string expected3 = "6\r\nWorld!\r\n";
    assert(chunk3 == expected3);
    std::cout << "  ✓ Chunk from buffer: " << chunk3.size() << " bytes" << std::endl;
}

void test_chunk_fromIOVec() {
    std::cout << "\nTesting Chunk::fromIOVec()..." << std::endl;

    // 测试1: 解析单个chunk
    std::string input1 = "5\r\nHello\r\n";
    std::vector<iovec> iovecs1(1);
    iovecs1[0].iov_base = const_cast<char*>(input1.data());
    iovecs1[0].iov_len = input1.size();

    std::string output1;
    auto result1 = Chunk::fromIOVec(iovecs1, output1);
    assert(result1.has_value());
    assert(result1.value().first == false);  // 不是最后一个chunk
    assert(result1.value().second == input1.size());  // 消费了所有字节
    assert(output1 == "Hello");
    std::cout << "  ✓ Single chunk parsed: \"" << output1 << "\"" << std::endl;

    // 测试2: 解析最后一个chunk
    std::string input2 = "0\r\n\r\n";
    std::vector<iovec> iovecs2(1);
    iovecs2[0].iov_base = const_cast<char*>(input2.data());
    iovecs2[0].iov_len = input2.size();

    std::string output2;
    auto result2 = Chunk::fromIOVec(iovecs2, output2);
    assert(result2.has_value());
    assert(result2.value().first == true);  // 是最后一个chunk
    assert(result2.value().second == input2.size());
    assert(output2.empty());
    std::cout << "  ✓ Last chunk parsed" << std::endl;

    // 测试3: 解析多个chunk
    std::string input3 = "5\r\nHello\r\n6\r\nWorld!\r\n";
    std::vector<iovec> iovecs3(1);
    iovecs3[0].iov_base = const_cast<char*>(input3.data());
    iovecs3[0].iov_len = input3.size();

    std::string output3;
    auto result3 = Chunk::fromIOVec(iovecs3, output3);
    assert(result3.has_value());
    assert(result3.value().first == false);  // 不是最后一个chunk
    assert(output3 == "HelloWorld!");  // 追加方式
    std::cout << "  ✓ Multiple chunks parsed: \"" << output3 << "\"" << std::endl;

    // 测试4: 数据不完整
    std::string input4 = "5\r\nHel";  // 不完整的chunk
    std::vector<iovec> iovecs4(1);
    iovecs4[0].iov_base = const_cast<char*>(input4.data());
    iovecs4[0].iov_len = input4.size();

    std::string output4;
    auto result4 = Chunk::fromIOVec(iovecs4, output4);
    assert(!result4.has_value());
    assert(result4.error().code() == kIncomplete);
    std::cout << "  ✓ Incomplete data detected" << std::endl;

    // 测试5: 跨iovec的chunk
    std::string part1 = "5\r\nHe";
    std::string part2 = "llo\r\n";
    std::vector<iovec> iovecs5(2);
    iovecs5[0].iov_base = const_cast<char*>(part1.data());
    iovecs5[0].iov_len = part1.size();
    iovecs5[1].iov_base = const_cast<char*>(part2.data());
    iovecs5[1].iov_len = part2.size();

    std::string output5;
    auto result5 = Chunk::fromIOVec(iovecs5, output5);
    assert(result5.has_value());
    assert(output5 == "Hello");
    std::cout << "  ✓ Cross-iovec chunk parsed: \"" << output5 << "\"" << std::endl;
}

void test_chunk_roundtrip() {
    std::cout << "\nTesting chunk roundtrip (toChunk -> fromIOVec)..." << std::endl;

    // 创建多个chunk
    std::string data1 = "First";
    std::string data2 = "Second";
    std::string data3 = "Third";
    std::string chunk1 = Chunk::toChunk(data1, false);
    std::string chunk2 = Chunk::toChunk(data2, false);
    std::string chunk3 = Chunk::toChunk(data3, false);
    std::string empty_data = "";
    std::string lastChunk = Chunk::toChunk(empty_data, true);

    // 合并所有chunk
    std::string allChunks = chunk1 + chunk2 + chunk3 + lastChunk;

    // 解析
    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(allChunks.data());
    iovecs[0].iov_len = allChunks.size();

    std::string output;
    auto result = Chunk::fromIOVec(iovecs, output);

    assert(result.has_value());
    assert(result.value().first == true);  // 最后一个chunk
    assert(output == "FirstSecondThird");
    std::cout << "  ✓ Roundtrip successful: \"" << output << "\"" << std::endl;
}

int main() {
    std::cout << "=== HTTP Chunk Unit Tests ===" << std::endl;

    try {
        test_chunk_toChunk();
        test_chunk_fromIOVec();
        test_chunk_roundtrip();

        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
