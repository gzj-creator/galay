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
    /**
     * @brief 字符串元数据结构
     * @details 管理字符串数据的原始指针、大小和容量信息
     */
    struct StringMetaData
    {
        StringMetaData() {};
        
        /**
         * @brief 从std::string构造
         * @param str 源字符串
         */
        StringMetaData(std::string& str);
        
        /**
         * @brief 从std::string_view构造
         * @param str 字符串视图
         */
        StringMetaData(const std::string_view& str);
        
        /**
         * @brief 从以\0结尾的C字符串构造
         * @param str C字符串指针
         */
        StringMetaData(const char* str);
        
        /**
         * @brief 从以\0结尾的字节数组构造
         * @param str 字节数组指针
         */
        StringMetaData(const uint8_t* str);
        
        /**
         * @brief 从原始指针和长度构造
         * @param str C字符串指针
         * @param length 字符串长度
         */
        StringMetaData(const char* str, size_t length);
        
        /**
         * @brief 从原始字节指针和长度构造
         * @param str 字节数组指针
         * @param length 数据长度
         */
        StringMetaData(const uint8_t* str, size_t length);
        
        /**
         * @brief 移动构造函数
         */
        StringMetaData(StringMetaData&& other);

        /**
         * @brief 移动赋值运算符
         */
        StringMetaData& operator=(StringMetaData&& other);

        ~StringMetaData();

        uint8_t* data = nullptr;    ///< 数据指针
        size_t size = 0;             ///< 当前数据大小
        size_t capacity = 0;         ///< 已分配容量
    };

    /**
     * @brief 分配指定长度的字符串内存
     * @param length 需要分配的长度
     * @return 分配的StringMetaData对象
     */
    StringMetaData mallocString(size_t length);
    
    /**
     * @brief 深拷贝字符串元数据
     * @param meta 源元数据
     * @return 拷贝的StringMetaData对象
     */
    StringMetaData deepCopyString(const StringMetaData& meta);
    
    /**
     * @brief 重新分配字符串内存
     * @param meta 字符串元数据引用
     * @param length 新的长度
     */
    void reallocString(StringMetaData& meta, size_t length);
    
    /**
     * @brief 清空字符串内容（保留已分配内存）
     * @param meta 字符串元数据引用
     */
    void clearString(StringMetaData& meta);
    
    /**
     * @brief 释放字符串内存
     * @param meta 字符串元数据引用
     */
    void freeString(StringMetaData& meta);

    /**
     * @brief 缓冲区类
     * @details 提供高效的内存缓冲区管理，支持动态扩容和移动语义
     */
    class Buffer 
    {
    public:
        /**
         * @brief 默认构造函数，创建空缓冲区
         */
        Buffer();
        
        /**
         * @brief 构造指定容量的缓冲区
         * @param capacity 初始容量
         */
        Buffer(size_t capacity);
        
        /**
         * @brief 从数据指针和大小构造缓冲区
         * @param data 数据指针
         * @param size 数据大小
         */
        Buffer(const void* data, size_t size);
        
        /**
         * @brief 从std::string构造缓冲区
         * @param str 源字符串
         */
        Buffer(const std::string& str);

        /**
         * @brief 清空缓冲区内容
         */
        void clear();
        
        /**
         * @brief 获取数据指针（可修改）
         * @return 数据指针
         */
        char *data();
        
        /**
         * @brief 获取数据指针（只读）
         * @return 常量数据指针
         */
        const char *data() const;
        
        /**
         * @brief 获取当前数据长度
         * @return 数据长度
         */
        size_t length() const;
        
        /**
         * @brief 获取缓冲区容量
         * @return 容量大小
         */
        size_t capacity() const;
        
        /**
         * @brief 调整缓冲区容量
         * @param capacity 新的容量大小
         */
        void resize(size_t capacity);
        
        /**
         * @brief 转换为std::string
         * @return 字符串副本
         */
        std::string toString() const;
        
        /**
         * @brief 转换为std::string_view（零拷贝）
         * @return 字符串视图
         */
        std::string_view toStringView() const;

        /**
         * @brief 移动赋值运算符
         */
        Buffer& operator=(Buffer&& other);

        ~Buffer();
        
        /**
         * @brief 与另一个缓冲区交换内容
         * @param other 另一个缓冲区
         */
        void swap(Buffer& other) {
            std::swap(m_data, other.m_data);
        }

    private:
        StringMetaData m_data;
    };
}

#endif