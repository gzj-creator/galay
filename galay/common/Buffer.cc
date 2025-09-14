#include "Buffer.h"

namespace galay
{
    StringMetaData::StringMetaData(std::string &str)
    {
        data = (uint8_t*)str.data();
        size = str.size();
        capacity = str.capacity();
    }

    StringMetaData::StringMetaData(const std::string_view &str)
    {
        data = (uint8_t*)str.data();
        size = str.size();
        capacity = str.length();
    }

    StringMetaData::StringMetaData(const char *str)
    {
        size = strlen(str);
        capacity = size;
        data = (uint8_t*)str;
    }

    StringMetaData::StringMetaData(const uint8_t *str)
    {
        size = strlen(reinterpret_cast<const char*>(str));
        capacity = size;
        data = (uint8_t*)str;
    }

    StringMetaData::StringMetaData(const char* str, size_t length)
    {
        if(length <= 0) throw std::invalid_argument("length must be greater than 0");
        size = strlen(reinterpret_cast<const char*>(str));
        capacity = length;
        size = length;
    }

    StringMetaData::StringMetaData(const uint8_t* str, size_t length)
    {
        if(length <= 0) throw std::invalid_argument("length must be greater than 0");
        size = strlen(reinterpret_cast<const char*>(str));
        capacity = length;
        size = length;
    }

    StringMetaData::StringMetaData(StringMetaData &&other)
        : data(other.data), size(other.size), capacity(other.capacity) 
    {
        other.data = nullptr;
        other.size = 0;
        other.capacity = 0;
    }

    StringMetaData& StringMetaData::operator=(StringMetaData&& other) 
    {
        if (this != &other) {
            data = other.data;
            size = other.size;
            capacity = other.capacity;
            other.data = nullptr;
            other.size = 0;
            other.capacity = 0;
        }
        return *this;
    }

    StringMetaData::~StringMetaData()
    {
        if(data) {
            data = nullptr;
            size = 0;
            capacity = 0;
        }
    }

    Buffer::Buffer(size_t capacity)
    {
        m_data = mallocString(capacity);
    }

    Buffer::Buffer(const void *data, size_t size)
    {
        m_data = mallocString(size);
        memcpy(m_data.data, data, size);
        m_data.size = size;
    }

    Buffer::Buffer(const std::string &str)
    {
        m_data = mallocString(str.size());
        memcpy(m_data.data, str.data(), str.size());
        m_data.size = str.size();
    }

    void Buffer::clear()
    {
        clearString(m_data);
    }

    char* Buffer::data()
    {
        return reinterpret_cast<char*>(m_data.data);
    }
    
    const char* Buffer::data() const
    {
        return reinterpret_cast<const char*>(m_data.data);
    }

    size_t Buffer::length() const
    {
        return m_data.size;
    }

    size_t Buffer::capacity() const
    {
        return m_data.capacity;
    }

    void Buffer::resize(size_t capacity)
    {
        reallocString(m_data, capacity);
    }

    std::string Buffer::toString() const
    {
        return std::string(reinterpret_cast<const char*>(m_data.data), m_data.size);
    }

    std::string_view Buffer::toStringView() const
    {
        return std::string_view(reinterpret_cast<const char*>(m_data.data), m_data.size);
    }

    Buffer::~Buffer()
    {
        clearString(m_data);
    }
}