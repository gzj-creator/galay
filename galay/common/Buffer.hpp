#ifndef GALAY_BUFFER_H
#define GALAY_BUFFER_H 

 #include <vector>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <algorithm>
#include <string>

namespace galay
{
    class Buffer {
    public:
        // 构造函数
        explicit Buffer(size_t initialCapacity = 1024)
            : m_capacity(initialCapacity), m_readPos(0), m_writePos(0) {
            if (initialCapacity == 0) {
                throw std::invalid_argument("Initial capacity must be greater than zero");
            }
            m_data.resize(initialCapacity);
        }

        // 从现有数据构造
        Buffer(const void* data, size_t size)
            : m_capacity(size), m_readPos(0), m_writePos(size) {
            if (data == nullptr && size > 0) {
                throw std::invalid_argument("Data pointer cannot be null when size > 0");
            }
            m_data.resize(size);
            if (size > 0) {
                std::memcpy(m_data.data(), data, size);
            }
        }

        // 从字符串构造
        explicit Buffer(const std::string& str)
            : m_capacity(str.size()), m_readPos(0), m_writePos(str.size()) {
            m_data.resize(str.size());
            std::memcpy(m_data.data(), str.data(), str.size());
        }

        // 禁止拷贝
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // 支持移动
        Buffer(Buffer&& other) noexcept
            : m_data(std::move(other.m_data))
            , m_capacity(other.m_capacity)
            , m_readPos(other.m_readPos)
            , m_writePos(other.m_writePos) {
            other.m_capacity = 0;
            other.m_readPos = 0;
            other.m_writePos = 0;
        }

        Buffer& operator=(Buffer&& other) noexcept {
            if (this != &other) {
                m_data = std::move(other.m_data);
                m_capacity = other.m_capacity;
                m_readPos = other.m_readPos;
                m_writePos = other.m_writePos;
                
                other.m_capacity = 0;
                other.m_readPos = 0;
                other.m_writePos = 0;
            }
            return *this;
        }

        // 写入原始数据
        size_t write(const void* data, size_t size) {
            if (size == 0) {
                return 0;
            }
            
            ensureCapacity(size);
            
            std::memcpy(m_data.data() + m_writePos, data, size);
            m_writePos += size;
            
            return size;
        }

        // 写入基本类型（模板方法）
        template<typename T>
        typename std::enable_if<std::is_fundamental<T>::value, size_t>::type
        write(T value) {
            ensureCapacity(sizeof(T));
            
            std::memcpy(m_data.data() + m_writePos, &value, sizeof(T));
            m_writePos += sizeof(T);
            
            return sizeof(T);
        }

        // 写入字符串
        size_t writeString(const std::string& str) {
            return write(str.data(), str.size());
        }

        // 写入带长度的字符串（先写入长度，再写入数据）
        size_t writeStringWithLength(const std::string& str) {
            uint32_t length = static_cast<uint32_t>(str.size());
            size_t total = write(length);
            total += write(str.data(), str.size());
            return total;
        }

        // 读取原始数据
        size_t read(void* dest, size_t size) {
            if (size == 0) {
                return 0;
            }
            
            size_t available = readableBytes();
            size_t toRead = std::min(size, available);
            
            if (toRead == 0) {
                return 0;
            }
            
            std::memcpy(dest, m_data.data() + m_readPos, toRead);
            m_readPos += toRead;
            
            return toRead;
        }

        // 读取基本类型
        template<typename T>
        typename std::enable_if<std::is_fundamental<T>::value, bool>::type
        read(T& value) {
            if (readableBytes() < sizeof(T)) {
                return false;
            }
            
            std::memcpy(&value, m_data.data() + m_readPos, sizeof(T));
            m_readPos += sizeof(T);
            
            return true;
        }

        // 读取字符串
        bool readString(std::string& str, size_t length) {
            if (readableBytes() < length) {
                return false;
            }
            
            str.assign(m_data.data() + m_readPos, length);
            m_readPos += length;
            
            return true;
        }

        // 读取带长度的字符串
        bool readStringWithLength(std::string& str) {
            uint32_t length = 0;
            if (!read(length)) {
                return false;
            }
            
            return readString(str, length);
        }

        // 查看数据但不移动读指针
        size_t peek(void* dest, size_t size) const {
            if (size == 0) {
                return 0;
            }
            
            size_t available = readableBytes();
            size_t toPeek = std::min(size, available);
            
            if (toPeek == 0) {
                return 0;
            }
            
            std::memcpy(dest, m_data.data() + m_readPos, toPeek);
            return toPeek;
        }

        // 查看基本类型但不移动读指针
        template<typename T>
        typename std::enable_if<std::is_fundamental<T>::value, bool>::type
        peek(T& value) const {
            if (readableBytes() < sizeof(T)) {
                return false;
            }
            
            std::memcpy(&value, m_data.data() + m_readPos, sizeof(T));
            return true;
        }

        // 跳过指定数量的字节
        size_t skip(size_t size) {
            size_t available = readableBytes();
            size_t toSkip = std::min(size, available);
            
            m_readPos += toSkip;
            return toSkip;
        }

        // 清空缓冲区
        void clear() {
            m_readPos = 0;
            m_writePos = 0;
        }

        // 重置读指针到开始位置
        void rewind() {
            m_readPos = 0;
        }

        // 获取可读字节数
        size_t readableBytes() const {
            return m_writePos - m_readPos;
        }

        // 获取可写字节数
        size_t writableBytes() const {
            return m_capacity - m_writePos;
        }

        // 获取容量
        size_t capacity() const {
            return m_capacity;
        }

        // 获取当前数据大小
        size_t size() const {
            return m_writePos;
        }

        // 获取读指针位置
        size_t readPosition() const {
            return m_readPos;
        }

        // 获取写指针位置
        size_t writePosition() const {
            return m_writePos;
        }

        // 设置读指针位置
        bool setReadPosition(size_t pos) {
            if (pos > m_writePos) {
                return false;
            }
            m_readPos = pos;
            return true;
        }

        // 设置写指针位置
        bool setWritePosition(size_t pos) {
            if (pos > m_capacity) {
                return false;
            }
            m_writePos = pos;
            return true;
        }

        // 获取底层数据指针
        const char* data() const {
            return m_data.data();
        }

        char* data() {
            return m_data.data();
        }

        // 获取可读数据指针
        const char* readData() const {
            return m_data.data() + m_readPos;
        }

        // 获取可写数据指针
        char* writeData() {
            return m_data.data() + m_writePos;
        }

        // 提取所有数据作为字符串
        std::string toString() const {
            return std::string(m_data.data(), m_writePos);
        }

        // 提取可读数据作为字符串
        std::string readToString() const {
            return std::string(m_data.data() + m_readPos, readableBytes());
        }

        // 确保有足够的容量
        void ensureCapacity(size_t required) {
            size_t available = writableBytes();
            if (available >= required) {
                return;
            }
            
            // 计算需要扩容的大小
            size_t newCapacity = m_capacity;
            while (newCapacity - m_writePos < required) {
                newCapacity = (newCapacity == 0) ? 1 : newCapacity * 2;
            }
            
            // 扩容
            if (newCapacity > m_capacity) {
                m_data.resize(newCapacity);
                m_capacity = newCapacity;
            }
        }

        // 紧凑缓冲区（将已读数据移除，将未读数据移动到开头）
        void compact() {
            if (m_readPos == 0) {
                return; // 不需要紧凑
            }
            
            size_t readable = readableBytes();
            if (readable > 0) {
                std::memmove(m_data.data(), m_data.data() + m_readPos, readable);
            }
            
            m_readPos = 0;
            m_writePos = readable;
        }

        // 交换两个缓冲区
        void swap(Buffer& other) {
            std::swap(m_data, other.m_data);
            std::swap(m_capacity, other.m_capacity);
            std::swap(m_readPos, other.m_readPos);
            std::swap(m_writePos, other.m_writePos);
        }

    private:
        std::vector<char> m_data;
        size_t m_capacity;
        size_t m_readPos;
        size_t m_writePos;
    };
}

#endif