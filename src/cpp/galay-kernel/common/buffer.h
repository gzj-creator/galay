/**
 * @file buffer.h
 * @brief 用于 I/O 操作的内存缓冲区与环形缓冲区
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 为 galay-kernel I/O 子系统提供缓冲区抽象：
 * - ByteMetaData：由 galay-utils 提供的底层字节元数据（指针、大小、容量）
 * - Buffer：支持动态增长和移动语义的线性字节缓冲区
 * - RingBuffer：由 galay-utils 提供的固定容量环形缓冲区，针对 scatter-gather I/O 优化
 */

#ifndef GALAY_BUFFER_H
#define GALAY_BUFFER_H

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <cstdint>
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-utils/cache/ring_buffer.hpp"

namespace galay::kernel
{
    using galay::utils::ByteMetaData;
    using galay::utils::RingBuffer;

    /**
     * @brief 支持动态增长和移动语义的线性字节缓冲区
     * @details 提供高效的内存缓冲区管理，支持动态重新分配和仅移动语义以避免拷贝。
     */
    class Buffer
    {
    public:
        /**
         * @brief 默认构造空缓冲区，不分配存储空间
         */
        Buffer();

        /**
         * @brief 以指定容量构造缓冲区
         * @param capacity 初始容量（字节）
         */
        Buffer(size_t capacity);

        /**
         * @brief 从原始数据拷贝构造
         * @param data 源指针
         * @param size 要拷贝的字节数
         */
        Buffer(const void* data, size_t size);

        /**
         * @brief 从 std::string 拷贝构造
         * @param str 源字符串
         */
        Buffer(const std::string& str);

        /**
         * @brief 清除缓冲区内容（内存清零，保留分配）
         */
        void clear();

        /**
         * @brief 获取数据的可变指针
         * @return 指向数据起始位置的 char 指针
         */
        char *data();

        /**
         * @brief 获取数据的常量指针
         * @return 指向数据起始位置的 const char 指针
         */
        const char *data() const;

        /**
         * @brief 获取当前存储的字节数
         * @return 数据大小（字节）
         */
        size_t length() const;

        /**
         * @brief 获取已分配的容量
         * @return 容量（字节）
         */
        size_t capacity() const;

        /**
         * @brief 通过 realloc 调整缓冲区大小
         * @param capacity 新容量（字节）；0 表示释放内存
         */
        void resize(size_t capacity);

        /**
         * @brief 将缓冲区内容拷贝为 std::string
         * @return 包含缓冲区数据的新字符串
         */
        std::string toString() const;

        /**
         * @brief 获取数据的零拷贝 string_view
         * @return 引用缓冲区内容的 string_view
         */
        std::string_view toStringView() const;

        /**
         * @brief 移动赋值运算符
         */
        Buffer& operator=(Buffer&& other);

        ~Buffer();

        /**
         * @brief 与另一个缓冲区交换内容
         * @param other 要交换的缓冲区
         */
        void swap(Buffer& other) {
            std::swap(m_data, other.m_data);
        }

    private:
        ByteMetaData m_data;
    };

}

#endif
