#ifndef GALAY_BUFFER_H
#define GALAY_BUFFER_H 

#include <memory>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <algorithm>
#include <string>

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

    class Buffer 
    {
    public:
        Buffer(size_t capacity);
        Buffer(const void* data, size_t size);
        Buffer(const std::string& str);

        void clear();
        char *data();
        const char *data() const;
        size_t length() const;
        size_t capacity() const;
        void resize(size_t capacity);
        std::string toString() const;
        std::string_view toStringView() const;
        ~Buffer();
        void swap(Buffer& other) {
            std::swap(m_data, other.m_data);
        }

    private:
        StringMetaData m_data;
    };
}

#endif