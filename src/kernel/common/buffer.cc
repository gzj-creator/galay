/**
 * @file buffer.cc
 * @brief Buffer 的实现
 * @author galay-kernel
 * @version 1.0.0
 */

#include "buffer.h"

namespace galay::kernel
{
    /**
     * @brief 默认构造空缓冲区，不分配存储空间
     */
    Buffer::Buffer()
    {
    }

    /**
     * @brief 以指定容量构造缓冲区
     * @param capacity 要分配的字节数
     */
    Buffer::Buffer(size_t capacity)
    {
        m_data = galay::utils::mallocBytes(capacity);
    }

    /**
     * @brief 从原始数据拷贝构造
     * @param data 源指针
     * @param size 要拷贝的字节数
     */
    Buffer::Buffer(const void *data, size_t size)
    {
        m_data = galay::utils::mallocBytes(size);
        memcpy(m_data.data, data, size);
        m_data.size = size;
    }

    /**
     * @brief 从 std::string 拷贝其内容构造
     * @param str 源字符串
     */
    Buffer::Buffer(const std::string &str)
    {
        m_data = galay::utils::mallocBytes(str.size());
        memcpy(m_data.data, str.data(), str.size());
        m_data.size = str.size();
    }

    /**
     * @brief 将缓冲区内容清零但不释放内存
     */
    void Buffer::clear()
    {
        galay::utils::clearBytes(m_data);
    }

    /**
     * @brief 获取缓冲区数据的可变指针
     * @return 指向数据起始位置的 char 指针
     */
    char* Buffer::data()
    {
        return reinterpret_cast<char*>(m_data.data);
    }

    /**
     * @brief 获取缓冲区数据的常量指针
     * @return 指向数据起始位置的 const char 指针
     */
    const char* Buffer::data() const
    {
        return reinterpret_cast<const char*>(m_data.data);
    }

    /**
     * @brief 获取当前存储的字节数
     * @return 数据大小（字节）
     */
    size_t Buffer::length() const
    {
        return m_data.size;
    }

    /**
     * @brief 获取已分配的容量
     * @return 容量（字节）
     */
    size_t Buffer::capacity() const
    {
        return m_data.capacity;
    }

    /**
     * @brief 通过 realloc 调整缓冲区容量
     * @param capacity 新容量（字节）；0 表示释放内存
     */
    void Buffer::resize(size_t capacity)
    {
        galay::utils::reallocBytes(m_data, capacity);
    }

    /**
     * @brief 将缓冲区内容拷贝为 std::string
     * @return 包含缓冲区数据的新字符串
     */
    std::string Buffer::toString() const
    {
        return std::string(reinterpret_cast<const char*>(m_data.data), m_data.size);
    }

    /**
     * @brief 获取缓冲区数据的零拷贝 string_view
     * @return 引用缓冲区内容的 string_view
     */
    std::string_view Buffer::toStringView() const
    {
        return std::string_view(reinterpret_cast<const char*>(m_data.data), m_data.size);
    }

    /**
     * @brief 移动赋值；释放当前存储并接管 other 的数据
     * @param other 源缓冲区（移后为空）
     * @return 本对象的引用
     */
    Buffer &Buffer::operator=(Buffer &&other)
    {
        if(this != &other) {
            galay::utils::freeBytes(m_data);
            m_data = other.m_data;
            other.m_data = {};
        }
        return *this;
    }

    /**
     * @brief 析构函数；将内部存储清零并释放
     */
    Buffer::~Buffer()
    {
        galay::utils::freeBytes(m_data);
    }

}
