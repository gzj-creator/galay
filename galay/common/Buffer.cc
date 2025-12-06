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

    Buffer::Buffer()
    {
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

    Buffer &Buffer::operator=(Buffer &&other)
    {
        if(this != &other) {
            m_data = std::move(other.m_data);
        }
        return *this;
    }

    Buffer::~Buffer()
    {
        clearString(m_data);
    }

    // ============ RingBuffer 实现（镜像缓冲区版本）============

    RingBuffer::RingBuffer(size_t capacity)
        : m_buffer(new uint8_t[capacity * 2])  // 分配2倍容量
        , m_capacity(capacity)
        , m_read_pos(0)
        , m_write_pos(0)
        , m_size(0)
    {
        if (capacity == 0) {
            throw std::invalid_argument("RingBuffer capacity must be greater than 0");
        }
    }

    RingBuffer::RingBuffer(RingBuffer&& other) noexcept
        : m_buffer(other.m_buffer)
        , m_capacity(other.m_capacity)
        , m_read_pos(other.m_read_pos)
        , m_write_pos(other.m_write_pos)
        , m_size(other.m_size)
    {
        other.m_buffer = nullptr;
        other.m_capacity = 0;
        other.m_read_pos = 0;
        other.m_write_pos = 0;
        other.m_size = 0;
    }

    RingBuffer& RingBuffer::operator=(RingBuffer&& other) noexcept
    {
        if (this != &other) {
            delete[] m_buffer;
            
            m_buffer = other.m_buffer;
            m_capacity = other.m_capacity;
            m_read_pos = other.m_read_pos;
            m_write_pos = other.m_write_pos;
            m_size = other.m_size;
            
            other.m_buffer = nullptr;
            other.m_capacity = 0;
            other.m_read_pos = 0;
            other.m_write_pos = 0;
            other.m_size = 0;
        }
        return *this;
    }

    RingBuffer::~RingBuffer()
    {
        delete[] m_buffer;
    }

    void RingBuffer::updateMirror()
    {
        // 将前半部分的数据镜像到后半部分
        // 只需要镜像可能被读取的部分
        if (m_read_pos + m_size > m_capacity) {
            // 数据会跨越边界，需要镜像
            size_t mirror_start = m_read_pos;
            size_t mirror_size = m_capacity - m_read_pos;
            std::memcpy(m_buffer + m_capacity + mirror_start, 
                       m_buffer + mirror_start, 
                       mirror_size);
        }
    }

    std::pair<char*, size_t> RingBuffer::getWriteBuffer()
    {
        if (m_size == m_capacity) {
            return {nullptr, 0};
        }

        // 写位置对capacity取模
        size_t write_idx = m_write_pos % m_capacity;
        size_t available = m_capacity - m_size;
        
        // 返回写指针，可写到capacity末尾
        size_t writable_size = std::min(available, m_capacity - write_idx);
        
        return {reinterpret_cast<char*>(m_buffer + write_idx), writable_size};
    }

    std::pair<const char*, size_t> RingBuffer::getReadBuffer() const
    {
        if (m_size == 0) {
            return {nullptr, 0};
        }

        // 读位置对capacity取模
        size_t read_idx = m_read_pos % m_capacity;
        
        // 由于使用了镜像，即使环绕也能保证连续读取
        // 直接返回从read_idx开始的m_size字节
        return {reinterpret_cast<const char*>(m_buffer + read_idx), m_size};
    }

    void RingBuffer::produce(size_t n)
    {
        if (n == 0) {
            return;
        }

        size_t available = m_capacity - m_size;
        if (n > available) {
            n = available;
        }

        m_write_pos += n;
        m_size += n;
        
        // 更新镜像
        updateMirror();
    }

    void RingBuffer::consume(size_t n)
    {
        if (n == 0) {
            return;
        }

        if (n > m_size) {
            n = m_size;
        }

        m_read_pos += n;
        m_size -= n;

        // 如果读完了，重置指针到开头以避免溢出
        if (m_size == 0) {
            m_read_pos = 0;
            m_write_pos = 0;
        }
        // 当read_pos太大时，整体向前移动
        else if (m_read_pos >= m_capacity) {
            m_read_pos -= m_capacity;
            m_write_pos -= m_capacity;
        }
    }

    void RingBuffer::resize(size_t new_capacity)
    {
        if (new_capacity < m_size) {
            throw std::invalid_argument("New capacity must be >= current size");
        }

        if (new_capacity == m_capacity) {
            return;
        }

        uint8_t* new_buffer = new uint8_t[new_capacity * 2];  // 分配2倍容量

        // 复制现有数据到新缓冲区开头
        if (m_size > 0) {
            size_t read_idx = m_read_pos % m_capacity;
            
            if (read_idx + m_size <= m_capacity) {
                // 数据连续
                std::memcpy(new_buffer, m_buffer + read_idx, m_size);
            } else {
                // 数据环绕
                size_t first_chunk = m_capacity - read_idx;
                std::memcpy(new_buffer, m_buffer + read_idx, first_chunk);
                std::memcpy(new_buffer + first_chunk, m_buffer, m_size - first_chunk);
            }
            
            // 设置镜像
            if (m_size > 0) {
                std::memcpy(new_buffer + new_capacity, new_buffer, std::min(m_size, new_capacity));
            }
        }

        delete[] m_buffer;
        m_buffer = new_buffer;
        m_capacity = new_capacity;
        m_read_pos = 0;
        m_write_pos = m_size;
    }

    size_t RingBuffer::readable() const
    {
        return m_size;
    }

    size_t RingBuffer::writable() const
    {
        return m_capacity - m_size;
    }

    size_t RingBuffer::capacity() const
    {
        return m_capacity;
    }

    bool RingBuffer::empty() const
    {
        return m_size == 0;
    }

    void RingBuffer::clear()
    {
        m_read_pos = 0;
        m_write_pos = 0;
        m_size = 0;
    }
}