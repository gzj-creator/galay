#ifndef GALAY_BYTES_H
#define GALAY_BYTES_H

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <variant>
#include "galay/common/Buffer.h"

namespace galay 
{ 
    /**
     * @brief 字节数据容器类
     * @details 高效的字节序列容器，支持移动语义，避免不必要的拷贝
     *          适用于网络I/O和数据传输场景
     */
    class Bytes 
    {
    public:
        /**
         * @brief 默认构造函数，创建空字节容器
         */
        Bytes() {};
        
        /**
         * @brief 从std::string构造
         * @param str 源字符串
         */
        Bytes(std::string& str);
        
        /**
         * @brief 从std::string移动构造
         * @param str 源字符串（移动语义）
         */
        Bytes(std::string&& str);
        
        /**
         * @brief 从以\0结尾的C字符串构造
         * @param str C字符串指针
         */
        Bytes(const char* str);
        
        /**
         * @brief 从以\0结尾的字节数组构造
         * @param str 字节数组指针
         */
        Bytes(const uint8_t* str);
        
        /**
         * @brief 从原始指针和长度构造
         * @param str C字符串指针
         * @param length 字符串长度
         */
        Bytes(const char* str, size_t length);
        
        /**
         * @brief 从字节指针和长度构造
         * @param str 字节数组指针
         * @param length 数据长度
         */
        Bytes(const uint8_t* str, size_t length);
        
        /**
         * @brief 构造指定容量的空字节容器
         * @param capacity 初始容量
         */
        Bytes(size_t capacity);
        
        /**
         * @brief 移动构造函数
         */
        Bytes(Bytes&& other) noexcept;
        
        /**
         * @brief 删除的拷贝构造函数（避免不必要的拷贝）
         */
        Bytes(const Bytes& other) = delete;
        
        /**
         * @brief 移动赋值运算符
         */
        Bytes& operator=(Bytes&& other) noexcept;
        
        /**
         * @brief 删除的拷贝赋值运算符（避免不必要的拷贝）
         */
        Bytes& operator=(const Bytes& other) = delete;

        /**
         * @brief 从std::string创建Bytes，浅拷贝
         * @param str 源字符串
         * @return Bytes对象
         */
        static Bytes fromString(std::string& str);
        
        /**
         * @brief 从std::string_view创建Bytes，浅拷贝
         * @param str 字符串视图
         * @return Bytes对象
         */
        static Bytes fromString(const std::string_view& str);
        
        /**
         * @brief 从C字符串创建指定容量的Bytes，浅拷贝
         * @param str C字符串指针
         * @param length 字符串长度
         * @param capacity 容量
         * @return Bytes对象
         */
        static Bytes fromCString(const char* str, size_t length, size_t capacity);

        /**
         * @brief 获取数据指针（uint8_t类型）
         * @return 常量数据指针
         */
        const uint8_t* data() const noexcept;
        
        /**
         * @brief 获取数据指针（C字符串类型）
         * @return 常量C字符串指针
         */
        const char* c_str() const noexcept;
        
        /**
         * @brief 获取数据大小
         * @return 字节数
         */
        size_t size() const noexcept;
        
        /**
         * @brief 获取容量
         * @return 已分配容量
         */
        size_t capacity() const noexcept;
        
        /**
         * @brief 检查是否为空
         * @return 是否为空
         */
        bool empty() const noexcept;
        
        /**
         * @brief 清空数据（保留已分配容量）
         */
        void clear() noexcept;
        
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
         * @brief 相等比较运算符
         * @param other 另一个Bytes对象
         * @return 是否相等
         */
        bool operator==(const Bytes& other) const;
        
        /**
         * @brief 不等比较运算符
         * @param other 另一个Bytes对象
         * @return 是否不等
         */
        bool operator!=(const Bytes& other) const;
    private:
        std::variant<StringMetaData, std::string> m_string;
    };
}


#endif