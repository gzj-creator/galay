#ifndef GALAY_BYTES_HPP
#define GALAY_BYTES_HPP 

#include <cstring>
#include <algorithm>
#include <memory>
#include <utility>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace galay 
{ 


    class Bytes {
        friend class BytesVisitor;
    public:
        // 默认构造函数
        Bytes() = default;
        // 从原始指针和长度构造
        Bytes(const char* data, size_t length);
        Bytes(const uint8_t* data, size_t length);
        explicit Bytes(size_t capacity);
        // 移动构造函数
        Bytes(Bytes&& other) noexcept;
        // 拷贝构造函数
        Bytes(const Bytes& other) = delete;
        // 析构函数
        ~Bytes();
        // 移动赋值运算符
        Bytes& operator=(Bytes&& other) noexcept;
        // 拷贝赋值运算符
        Bytes& operator=(const Bytes& other) = delete;
        // 获取数据指针
        const uint8_t* data() const noexcept { return mData; }
        uint8_t* data() noexcept { return mData; }
        // 获取数据大小
        size_t size() const noexcept { return mSize; }
        // 获取容量
        size_t capacity() const noexcept { return mCapacity; }
        // 是否为空
        bool empty() const noexcept { return mSize == 0; }
        // 清空数据（保留容量）
        void clear() noexcept { mSize = 0; }
        // 释放所有内存
        void release();
        // 重新分配内存
        void reallocate(size_t newSize);
        // 重新分配内存并填充0（类似calloc）
        void reallocateZero(size_t newSize);
        // 确保有足够容量（类似vector的reserve）
        void reserve(size_t newCapacity);

        // 追加数据
        void append(const uint8_t* data, size_t length);

        // 切片操作（深拷贝）
        Bytes slice(size_t pos, size_t len) const;

        std::string toString() const;

        // 比较操作
        bool operator==(const Bytes& other) const {
            if (mSize != other.mSize) return false;
            return std::memcmp(mData, other.mData, mSize) == 0;
        }

        bool operator!=(const Bytes& other) const {
            return !(*this == other);
        }


    private:
        uint8_t* mData = nullptr;
        size_t mSize = 0;
        size_t mCapacity = 0;
    };

    class BytesVisitor
    {
    public:
        explicit BytesVisitor(Bytes& bytes);
        size_t& size();
        size_t& Capacity();
    private:
        Bytes& m_bytes;
    };


}


#endif