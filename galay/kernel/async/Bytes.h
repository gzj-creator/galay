#ifndef GALAY_BYTES_H
#define GALAY_BYTES_H

#include <cstring>
#include <algorithm>
#include <memory>
#include <utility>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <variant>
#include <string_view>

namespace galay 
{ 
    struct StringMetaData
    {
        StringMetaData() {};
        StringMetaData(std::string& str);
        StringMetaData(const std::string_view& str);
        //以\0结尾的字符串构造
        StringMetaData(const char* str);
        StringMetaData(const uint8_t* str);
        // 从原始指针和长度构造
        StringMetaData(const char* str, size_t length);
        StringMetaData(const uint8_t* str, size_t length);
        StringMetaData(StringMetaData&& other);

        StringMetaData& operator=(StringMetaData&& other);

        ~StringMetaData();

        uint8_t* data = nullptr;
        size_t size = 0;
        size_t capacity = 0;
    };

    StringMetaData mallocString(size_t length);
    StringMetaData deepCopyString(const StringMetaData& meta);
    void reallocString(StringMetaData& meta, size_t length);
    void clearString(StringMetaData& meta);
    void freeString(StringMetaData& meta);

    class Bytes 
    {
    public:
        // 默认构造函数
        Bytes() {};
        Bytes(std::string& str);
        Bytes(std::string&& str);
        //以\0结尾的字符串构造
        Bytes(const char* str);
        Bytes(const uint8_t* str);
        // 从原始指针和长度构造
        Bytes(const char* str, size_t length);
        Bytes(const uint8_t* str, size_t length);
        Bytes(size_t capacity);
        // 移动构造函数
        Bytes(Bytes&& other) noexcept;
        // 拷贝构造函数
        Bytes(const Bytes& other) = delete;
        // 移动赋值运算符
        Bytes& operator=(Bytes&& other) noexcept;
        // 拷贝赋值运算符
        Bytes& operator=(const Bytes& other) = delete;

        static Bytes fromString(std::string& str);
        static Bytes fromString(const std::string_view& str);
        static Bytes fromCString(const char* str, size_t length, size_t capacity);

        // 获取数据指针
        const uint8_t* data() const noexcept;
        const char* c_str() const noexcept;
        // 获取数据大小
        size_t size() const noexcept;
        // 获取容量
        size_t capacity() const noexcept;
        // 是否为空
        bool empty() const noexcept;
        // 清空数据（保留容量）
        void clear() noexcept;
        std::string toString() const;
        std::string_view toStringView() const;
        // 比较操作
        bool operator==(const Bytes& other) const;
        bool operator!=(const Bytes& other) const;
    private:
        std::variant<StringMetaData, std::string> m_string;
    };
}


#endif