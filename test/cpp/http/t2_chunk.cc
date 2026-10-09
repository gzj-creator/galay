/**
 * @file test_chunk.cc
 * @brief HTTP Chunk 类单元测试
 */

#include <iostream>
#include <cassert>
#include <cstring>
#include <array>
#include <cstddef>
#include <string>
#include <galay/cpp/galay-http/protoc/http_chunk.h>

using namespace galay::http;

void test_chunk_to_chunk() {
    std::cout << "Testing Chunk::toChunk()..." << std::endl;

    // 测试1: 普通chunk
    std::string data1 = "Hello";
    std::string chunk1 = Chunk::to_chunk(data1, false);
    std::string expected1 = "5\r\nHello\r\n";
    assert(chunk1 == expected1);
    std::cout << "  ✓ Normal chunk: " << chunk1.size() << " bytes" << std::endl;

    // 测试2: 最后一个chunk
    std::string empty_data = "";
    std::string chunk2 = Chunk::to_chunk(empty_data, true);
    std::string expected2 = "0\r\n\r\n";
    assert(chunk2 == expected2);
    std::cout << "  ✓ Last chunk: " << chunk2.size() << " bytes" << std::endl;

    // 测试3: 从buffer创建
    const char* buffer = "World!";
    std::string chunk3 = Chunk::to_chunk(buffer, 6, false);
    std::string expected3 = "6\r\nWorld!\r\n";
    assert(chunk3 == expected3);
    std::cout << "  ✓ Chunk from buffer: " << chunk3.size() << " bytes" << std::endl;
}

bool test_chunk_output_boundaries() {
    std::cout << "\nTesting chunk output boundaries and binary payloads..." << std::endl;

    struct ChunkLengthCase {
        std::size_t length;
        const char* hex_length;
    };
    constexpr std::array<ChunkLengthCase, 12> length_cases{{
        {0, "0"},
        {1, "1"},
        {10, "a"},
        {11, "b"},
        {15, "f"},
        {16, "10"},
        {255, "ff"},
        {256, "100"},
        {4095, "fff"},
        {4096, "1000"},
        {65535, "ffff"},
        {65536, "10000"},
    }};

    for (const auto& length_case : length_cases) {
        for (const bool binary : std::array<bool, 2>{false, true}) {
            std::string payload(length_case.length, 'x');
            if (binary) {
                for (std::size_t index = 0; index < payload.size(); ++index) {
                    payload[index] = static_cast<char>(index % 256);
                }
            }
            const std::string expected = std::string(length_case.hex_length)
                + "\r\n" + payload + "\r\n";
            const std::string string_chunk = Chunk::to_chunk(payload, false);
            const std::string buffer_chunk = Chunk::to_chunk(payload.data(), payload.size(), false);
            if (string_chunk != expected || buffer_chunk != expected) {
                std::cerr << "Chunk output mismatch: length=" << length_case.length
                          << ", binary=" << binary << std::endl;
                return false;
            }

            // End markers must discard a supplied payload, including binary data.
            const std::string string_end = Chunk::to_chunk(payload, true);
            const std::string buffer_end = Chunk::to_chunk(payload.data(), payload.size(), true);
            if (string_end != "0\r\n\r\n" || buffer_end != "0\r\n\r\n") {
                std::cerr << "Chunk end marker mismatch: length=" << length_case.length
                          << ", binary=" << binary << std::endl;
                return false;
            }
        }
    }

    std::cout << "  ✓ 96 output checks passed, including lowercase hex and binary data"
              << std::endl;
    return true;
}

void test_chunk_from_io_vec() {
    std::cout << "\nTesting Chunk::fromIOVec()..." << std::endl;

    // 测试1: 解析单个chunk
    std::string input1 = "5\r\nHello\r\n";
    std::vector<iovec> iovecs1(1);
    iovecs1[0].iov_base = const_cast<char*>(input1.data());
    iovecs1[0].iov_len = input1.size();

    std::string output1;
    auto result1 = Chunk::from_io_vec(iovecs1, output1);
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
    auto result2 = Chunk::from_io_vec(iovecs2, output2);
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
    auto result3 = Chunk::from_io_vec(iovecs3, output3);
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
    auto result4 = Chunk::from_io_vec(iovecs4, output4);
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
    auto result5 = Chunk::from_io_vec(iovecs5, output5);
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
    std::string chunk1 = Chunk::to_chunk(data1, false);
    std::string chunk2 = Chunk::to_chunk(data2, false);
    std::string chunk3 = Chunk::to_chunk(data3, false);
    std::string empty_data = "";
    std::string lastChunk = Chunk::to_chunk(empty_data, true);

    // 合并所有chunk
    std::string allChunks = chunk1 + chunk2 + chunk3 + lastChunk;

    // 解析
    std::vector<iovec> iovecs(1);
    iovecs[0].iov_base = const_cast<char*>(allChunks.data());
    iovecs[0].iov_len = allChunks.size();

    std::string output;
    auto result = Chunk::from_io_vec(iovecs, output);

    assert(result.has_value());
    assert(result.value().first == true);  // 最后一个chunk
    assert(output == "FirstSecondThird");
    std::cout << "  ✓ Roundtrip successful: \"" << output << "\"" << std::endl;
}

int main() {
    std::cout << "=== HTTP Chunk Unit Tests ===" << std::endl;

    try {
        test_chunk_to_chunk();
        if (!test_chunk_output_boundaries()) {
            return 1;
        }
        test_chunk_from_io_vec();
        test_chunk_roundtrip();

        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
