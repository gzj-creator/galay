#include <iostream>
#include <cstring>
#include "galay-redis/protoc/redis_protocol.h"

using namespace galay::redis::protocol;

// 测试协议解析器
void test_parser() {
    std::cout << "=== Testing RESP Parser ===" << std::endl;

    RespParser parser;

    // 测试简单字符串
    {
        const char* data = "+OK\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isSimpleString()) {
            std::cout << "✓ Simple String: " << result->second.asString() << std::endl;
        } else {
            std::cout << "✗ Simple String test failed" << std::endl;
        }
    }

    // 测试错误
    {
        const char* data = "-ERR unknown command\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isError()) {
            std::cout << "✓ Error: " << result->second.asString() << std::endl;
        } else {
            std::cout << "✗ Error test failed" << std::endl;
        }
    }

    // 测试整数
    {
        const char* data = ":1000\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isInteger()) {
            std::cout << "✓ Integer: " << result->second.asInteger() << std::endl;
        } else {
            std::cout << "✗ Integer test failed" << std::endl;
        }
    }

    // 测试批量字符串
    {
        const char* data = "$6\r\nfoobar\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isBulkString()) {
            std::cout << "✓ Bulk String: " << result->second.asString() << std::endl;
        } else {
            std::cout << "✗ Bulk String test failed" << std::endl;
        }
    }

    // 测试空值
    {
        const char* data = "$-1\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isNull()) {
            std::cout << "✓ Null value parsed correctly" << std::endl;
        } else {
            std::cout << "✗ Null value test failed" << std::endl;
        }
    }

    // 测试数组
    {
        const char* data = "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isArray()) {
            auto& arr = result->second.asArray();
            std::cout << "✓ Array with " << arr.size() << " elements:" << std::endl;
            for (const auto& elem : arr) {
                if (elem.isBulkString()) {
                    std::cout << "  - " << elem.asString() << std::endl;
                }
            }
        } else {
            std::cout << "✗ Array test failed" << std::endl;
        }
    }

    // 测试嵌套数组
    {
        const char* data = "*2\r\n*2\r\n:1\r\n:2\r\n*2\r\n:3\r\n:4\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isArray()) {
            std::cout << "✓ Nested array parsed correctly" << std::endl;
        } else {
            std::cout << "✗ Nested array test failed" << std::endl;
        }
    }

    // 测试RESP3布尔值
    {
        const char* data = "#t\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isBoolean()) {
            std::cout << "✓ Boolean: " << (result->second.asBoolean() ? "true" : "false") << std::endl;
        } else {
            std::cout << "✗ Boolean test failed" << std::endl;
        }
    }

    // 测试RESP3双精度浮点数
    {
        const char* data = ",1.23\r\n";
        auto result = parser.parse(data, strlen(data));
        if (result && result->second.isDouble()) {
            std::cout << "✓ Double: " << result->second.asDouble() << std::endl;
        } else {
            std::cout << "✗ Double test failed" << std::endl;
        }
    }

    std::cout << std::endl;
}

// 测试协议编码器
void test_encoder() {
    std::cout << "=== Testing RESP Encoder ===" << std::endl;

    RespEncoder encoder;

    // 测试简单字符串
    {
        std::string result = encoder.encodeSimpleString("OK");
        std::cout << "Simple String: " << result;
    }

    // 测试错误
    {
        std::string result = encoder.encodeError("ERR unknown command");
        std::cout << "Error: " << result;
    }

    // 测试整数
    {
        std::string result = encoder.encodeInteger(1000);
        std::cout << "Integer: " << result;
    }

    // 测试批量字符串
    {
        std::string result = encoder.encodeBulkString("foobar");
        std::cout << "Bulk String: " << result;
    }

    // 测试空值
    {
        std::string result = encoder.encodeNull();
        std::cout << "Null: " << result;
    }

    // 测试数组
    {
        std::vector<std::string> arr = {"foo", "bar", "baz"};
        std::string result = encoder.encodeArray(arr);
        std::cout << "Array: " << result;
    }

    // 测试命令
    {
        std::string result = encoder.encodeCommand("SET", {"mykey", "myvalue"});
        std::cout << "Command: " << result;
    }

    std::cout << std::endl;
}

// 测试编码解码往返
void test_round_trip() {
    std::cout << "=== Testing Round Trip (Encode -> Decode) ===" << std::endl;

    RespEncoder encoder;
    RespParser parser;

    // 测试字符串往返
    {
        std::string original = "Hello, Redis!";
        std::string encoded = encoder.encodeBulkString(original);
        auto result = parser.parse(encoded.c_str(), encoded.length());

        if (result && result->second.isBulkString() && result->second.asString() == original) {
            std::cout << "✓ String round trip successful" << std::endl;
        } else {
            std::cout << "✗ String round trip failed" << std::endl;
        }
    }

    // 测试整数往返
    {
        int64_t original = 12345;
        std::string encoded = encoder.encodeInteger(original);
        auto result = parser.parse(encoded.c_str(), encoded.length());

        if (result && result->second.isInteger() && result->second.asInteger() == original) {
            std::cout << "✓ Integer round trip successful" << std::endl;
        } else {
            std::cout << "✗ Integer round trip failed" << std::endl;
        }
    }

    // 测试命令编码后可以被解析
    {
        std::string cmd = encoder.encodeCommand("GET", {"mykey"});
        auto result = parser.parse(cmd.c_str(), cmd.length());

        if (result && result->second.isArray() && result->second.asArray().size() == 2) {
            std::cout << "✓ Command round trip successful" << std::endl;
        } else {
            std::cout << "✗ Command round trip failed" << std::endl;
        }
    }

    std::cout << std::endl;
}


int main(int argc, char* argv[]) {
    std::cout << "Redis Protocol Parser and Client Test" << std::endl;
    std::cout << "======================================" << std::endl << std::endl;

    try {
        // 测试协议解析器
        test_parser();

        // 测试协议编码器
        test_encoder();

        // 测试往返编解码
        test_round_trip();

        // 测试Redis客户端（默认跳过，需要实际的Redis服务器）
        bool runOnlineTest = (argc > 1 && std::string(argv[1]) == "--online");
       
        std::cout << "All tests completed!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
