#include "Bytes.hpp"
#include <stdexcept>
#include <limits>
#include "galay/common/Log.h"

namespace galay
{
    Bytes::Bytes(const std::string &data)
    {
        if (!data.empty()) {
            mData = static_cast<uint8_t*>(malloc(data.size()));
            if (!mData) {
                LogError("malloc failed");
                throw std::bad_alloc();
            }
            mSize = data.size();
            mCapacity = data.size();
            std::memcpy(mData, data.data(), data.size());
        }
    }

    Bytes::Bytes(const std::string_view &data)
    {
        if (!data.empty()) {
            mData = static_cast<uint8_t*>(malloc(data.size()));
            if (!mData) {
                LogError("malloc failed");
                throw std::bad_alloc();
            }
            mSize = data.size();
            mCapacity = data.size();
            std::memcpy(mData, data.data(), data.size());
        }
    }

    Bytes::Bytes(const char *data)
    {
        if (data) {
            mSize = strlen(data);
            mCapacity = mSize;
            mData = static_cast<uint8_t*>(malloc(mSize));
            if (!mData) {
                LogError("malloc failed");
                throw std::bad_alloc();
            }
            std::memcpy(mData, data, mSize);
        }
    }

    Bytes::Bytes(const char* data, size_t length)
    {
        if (length > 0) {
            mData = static_cast<uint8_t*>(malloc(length));
            if (!mData) {
                LogError("malloc failed");
                throw std::bad_alloc();
            }
            mSize = length;
            mCapacity = length;
            std::memcpy(mData, data, length);
        }
    }

    Bytes::Bytes(const uint8_t* data, size_t length)
    {
        if (length > 0) {
            mData = static_cast<uint8_t*>(malloc(length));
            if (!mData) {
                LogError("malloc failed");
                throw std::bad_alloc();
            }
            mSize = length;
            mCapacity = length;
            std::memcpy(mData, data, length);
        }
    }

    Bytes::Bytes(size_t capacity)
    {
        if (capacity > 0)
        {
            mData = static_cast<uint8_t*>(malloc(capacity));
            if (!mData) {
                LogError("malloc failed");
                throw std::bad_alloc();
            }
            bzero(mData, capacity);
            mSize = 0;
            mCapacity = capacity;
        }
    }

    Bytes::Bytes(Bytes&& other) noexcept
        : mData(other.mData), mSize(other.mSize), mCapacity(other.mCapacity)
    {
        other.mData = nullptr;
        other.mSize = 0;
        other.mCapacity = 0;
    }

    Bytes::~Bytes() {
        if (mData) {
            free(mData);
            mData = nullptr;
        }
    }

    Bytes& Bytes::operator=(Bytes&& other) noexcept {
        if (this != &other) {
            if (mData)
            {
                free(mData);
                mData = nullptr;
            }
            mData = other.mData;
            mSize = other.mSize;
            mCapacity = other.mCapacity;
            other.mData = nullptr;
            other.mSize = 0;
            other.mCapacity = 0;
        }
        return *this;
    }

    void Bytes::release() {
        if (mData)
        {
            free(mData);
            mData = nullptr;
        }
        mData = nullptr;
        mSize = 0;
        mCapacity = 0;
    }

    void Bytes::reallocate(size_t newSize) {
        if (newSize == 0) {
            release();
            return;
        }

        if (newSize <= mCapacity) {
            mSize = newSize;
            return;
        }

        uint8_t* newData = static_cast<uint8_t*>(realloc(mData, newSize));
        if (!newData) {
            LogError("reallocate failed");
            throw std::bad_alloc();
        }

        mData = newData;
        mSize = newSize;
        mCapacity = newSize;
    }

    void Bytes::reallocateZero(size_t newSize) {
        if (newSize == 0) {
            release();
            return;
        }

        if (newSize <= mCapacity) {
            std::memset(mData, 0, newSize);
            mSize = newSize;
            return;
        }

        uint8_t* newData = static_cast<uint8_t*>(calloc(newSize, 1));
        if (!newData) {
            LogError("reallocate failed");
            throw std::bad_alloc();
        }

        if (mData) {
            std::memcpy(newData, mData, std::min(mSize, newSize));
            free(mData);
            mData = nullptr;
        }

        mData = newData;
        mSize = newSize;
        mCapacity = newSize;
    }

    void Bytes::reserve(size_t newCapacity) {
        if (newCapacity <= mCapacity) return;

        uint8_t* newData = static_cast<uint8_t*>(realloc(mData, newCapacity));
        if (!newData) {
            LogError("reallocate failed");
            throw std::bad_alloc();
        }

        mData = newData;
        mCapacity = newCapacity;
    }

    void Bytes::append(const uint8_t* data, size_t length) {
        if (length == 0) return;

        const size_t newSize = mSize + length;
        reserve(newSize);

        std::memcpy(mData + mSize, data, length);
        mSize = newSize;
    }

    Bytes Bytes::slice(size_t pos, size_t len) const {
        if (pos > mSize) {
            LogError("Bytes::slice position out of range");
            throw std::out_of_range("Bytes::slice - position out of range");
        }
        len = std::min(len, mSize - pos);
        return Bytes(mData + pos, len);
    }

    std::string Bytes::toString() const
    {
        if (mData)
        {
            return std::string(reinterpret_cast<char*>(mData), mSize);
        }
        return "";
    }

    std::string_view Bytes::toStringView() const
    {
        if(mData) {
            return std::string_view(reinterpret_cast<char*>(mData), mSize);
        }
        return std::string_view();
    }

    BytesVisitor::BytesVisitor(Bytes& bytes)
        :m_bytes(bytes)
    {

    }

    size_t& BytesVisitor::size()
    {
        return m_bytes.mSize;
    }

    size_t& BytesVisitor::Capacity()
    {
        return m_bytes.mCapacity;
    }

}