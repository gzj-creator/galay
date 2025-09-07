#include "Bytes.h"

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

    Bytes::Bytes(std::string &str)
    {
        std::string data(str);
        m_string = std::move(data);
    }

    Bytes::Bytes(std::string &&str)
    {
        m_string = std::move(str);
    }

    Bytes::Bytes(const char *str)
    {
        std::string data(str);
        m_string = std::move(data);
    }

    Bytes::Bytes(const uint8_t *str)
    {
        std::string data(reinterpret_cast<const char*>(str));
        m_string = std::move(data);
    }

    Bytes::Bytes(const char *str, size_t length)
    {
        std::string data(str, length);
        m_string = std::move(data);
    }
    
    Bytes::Bytes(const uint8_t *str, size_t length)
    {
        std::string data(reinterpret_cast<const char*>(str), length);
        m_string = std::move(data);
    }

    Bytes::Bytes(size_t capacity)
    {
        std::string data;
        data.reserve(capacity);
        m_string = std::move(data);
    }

    Bytes::Bytes(Bytes &&other) noexcept
    {
        m_string = std::move(other.m_string);
    }

    Bytes &Bytes::operator=(Bytes &&other) noexcept
    {
        m_string = std::move(other.m_string);
        return *this;
    }

    Bytes Bytes::fromString(std::string &str)
    {
        StringMetaData data;
        data.data = reinterpret_cast<uint8_t*>(str.data());
        data.size = str.size();
        data.capacity = str.capacity();
        Bytes bytes;
        bytes.m_string = std::move(data);
        return bytes;
    }

    Bytes Bytes::fromString(const std::string_view &str)
    {
        StringMetaData data;
        data.data = reinterpret_cast<uint8_t*>(const_cast<char*>(str.data()));
        data.size = str.size();
        data.capacity = str.size();
        Bytes bytes;
        bytes.m_string = std::move(data);
        return bytes;
    }

    Bytes Bytes::fromCString(const char *str, size_t length, size_t capacity)
    {
        StringMetaData data;
        data.data = reinterpret_cast<uint8_t*>(const_cast<char*>(str));
        data.size = length;
        data.capacity = capacity;
        Bytes bytes;
        bytes.m_string = std::move(data);
        return bytes;
    }

    const uint8_t* Bytes::data() const noexcept
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            return std::get<StringMetaData>(m_string).data;
        } else if(std::holds_alternative<std::string>(m_string)) {
            return reinterpret_cast<const uint8_t*>(std::get<std::string>(m_string).data());
        }
        return nullptr;
    }

    const char* Bytes::c_str() const noexcept
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            auto& data = std::get<StringMetaData>(m_string);
            if(data.data[data.size - 1] != '\0') {
                data.data[data.size] = '\0';
            }
            return reinterpret_cast<const char*>(data.data);
        } else if(std::holds_alternative<std::string>(m_string)) {
            return std::get<std::string>(m_string).c_str();
        }
        return nullptr;
    }

    size_t Bytes::size() const noexcept
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            return std::get<StringMetaData>(m_string).size;
        } else if(std::holds_alternative<std::string>(m_string)) {
            return std::get<std::string>(m_string).size();
        }
        return 0;
    }

    size_t Bytes::capacity() const noexcept
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            return std::get<StringMetaData>(m_string).capacity;
        } else if(std::holds_alternative<std::string>(m_string)) {
            return std::get<std::string>(m_string).capacity();
        }
        return 0;
    }

    bool Bytes::empty() const noexcept
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            return std::get<StringMetaData>(m_string).size == 0;
        } else if(std::holds_alternative<std::string>(m_string)) {
            return std::get<std::string>(m_string).empty();
        }
        return true;
    }

    void Bytes::clear() noexcept
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            auto& str = std::get<StringMetaData>(m_string);
            str.data = nullptr;
            str.size = 0;
            str.capacity = 0;
        } else if(std::holds_alternative<std::string>(m_string)) {
            std::get<std::string>(m_string).clear();
        }
    }

    std::string Bytes::toString() const
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            auto& str = std::get<StringMetaData>(m_string);
            return std::string(reinterpret_cast<char*>(str.data), str.size);
        } else if(std::holds_alternative<std::string>(m_string)) {
            return std::get<std::string>(m_string);
        }
        return "";
    }

    std::string_view Bytes::toStringView() const
    {
        if(std::holds_alternative<StringMetaData>(m_string)) {
            auto& str = std::get<StringMetaData>(m_string);
            return std::string_view(reinterpret_cast<const char*>(str.data), str.size);
        } else if(std::holds_alternative<std::string>(m_string)) {
            return std::get<std::string>(m_string);
        }
        return std::string_view();
    }

    bool Bytes::operator==(const Bytes &other) const
    {
        if(std::holds_alternative<StringMetaData>(m_string) && std::holds_alternative<StringMetaData>(other.m_string)) {
            auto& str = std::get<StringMetaData>(m_string);
            auto& str2 = std::get<StringMetaData>(other.m_string);
            return str.size == str2.size && std::memcmp(str.data, str2.data, str.size) == 0;
        } else if(std::holds_alternative<std::string>(m_string) && std::holds_alternative<std::string>(other.m_string)) {
            auto& str = std::get<std::string>(m_string);
            auto& str2 = std::get<std::string>(other.m_string);
            return str == str2;
        } else if(std::holds_alternative<std::string>(m_string) && std::holds_alternative<StringMetaData>(other.m_string)) {
            auto& str = std::get<std::string>(m_string);
            auto& str2 = std::get<StringMetaData>(other.m_string);
            return str.size() == str2.size && std::memcmp(str.c_str(), str2.data, str.size()) == 0;
        } else if(std::holds_alternative<StringMetaData>(m_string) && std::holds_alternative<std::string>(other.m_string)) {
            auto& str = std::get<StringMetaData>(m_string);
            auto& str2 = std::get<std::string>(other.m_string);
            return str.size == str2.size() && std::memcmp(str.data, str2.c_str(), str.size) == 0;
        }
        return false;
    }

    bool Bytes::operator!=(const Bytes &other) const
    {
        return !operator==(other);
    }

    StringMetaData mallocString(size_t length)
    {
        StringMetaData metaData;
        metaData.capacity = length;
        metaData.data = (uint8_t*)malloc(length);
        metaData.size = 0;
        return metaData;
    }

    StringMetaData deepCopyString(const StringMetaData& meta)
    {
        StringMetaData metaData;
        metaData = mallocString(meta.capacity);
        metaData.size = meta.size;
        memcpy(metaData.data, meta.data, meta.size);
        return metaData;
    }

    void reallocString(StringMetaData &meta, size_t length)
    {
        if(length == 0) {
            // 释放内存
            if (meta.data) {
                free(meta.data);
                meta.data = nullptr;
            }
            meta.size = 0;
            meta.capacity = 0;
            return;
        }
        if(meta.size > length) {
            meta.size = length;
        }
        meta.capacity = length;
        meta.data = (uint8_t*)realloc(meta.data, length);
        if (meta.data == nullptr)
        {
            throw std::bad_alloc();
        }
    }

    void clearString(StringMetaData &meta)
    {
        meta.size = 0;
        memset(meta.data, 0, meta.capacity);
    }

    void freeString(StringMetaData &meta)
    {
        if(meta.data != nullptr) {
            free(meta.data);
            meta.data = nullptr;
            meta.capacity = 0;
            meta.size = 0;
        }
    }
}