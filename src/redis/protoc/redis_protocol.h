/**
 * @file redis_protocol.h
 * @brief Redis RESP 协议解析与编码
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 实现 Redis RESP2/RESP3 协议的解析器（RespParser）、编码器（RespEncoder）
 *          和回复类型（RedisReply），支持所有 RESP 数据类型的解析和编码。
 */

#ifndef GALAY_REDIS_PROTOCOL_H
#define GALAY_REDIS_PROTOCOL_H

#include <string>
#include <vector>
#include <variant>
#include <memory>
#include <optional>
#include <expected>
#include <cstdint>
#include <string_view>
#include <charconv>
#include <type_traits>
#include <span>
#include <array>

namespace galay::redis::protocol
{
    /**
     * @brief RESP 协议类型枚举
     * @details 包含 RESP2 的基本类型和 RESP3 的扩展类型
     */
    enum class RespType
    {
        SimpleString,   ///< 简单字符串 (+)
        Error,          ///< 错误 (-)
        Integer,        ///< 整数 (:)
        BulkString,     ///< 批量字符串 ($)
        Array,          ///< 数组 (*)
        Null,           ///< 空值 ($-1\r\n 或 *-1\r\n)
        // RESP3扩展类型
        Double,         ///< 双精度浮点数 (,)
        Boolean,        ///< 布尔值 (#)
        BlobError,      ///< 批量错误 (!)
        VerbatimString, ///< 原义字符串 (=)
        BigNumber,      ///< 大数字 (()
        Map,            ///< 映射 (%)
        Set,            ///< 集合 (~)
        Push            ///< 推送 (>)
    };

    // Redis响应值的前向声明
    class RedisReply;

    /**
     * @brief RESP 值类型的变体
     * @details 使用 std::variant 存储所有可能的 RESP 数据类型
     */
    using RespData = std::variant<
        std::string,                           ///< SimpleString, Error, BulkString
        int64_t,                               ///< Integer
        double,                                ///< Double
        bool,                                  ///< Boolean
        std::vector<RedisReply>,               ///< Array, Set, Push
        std::vector<std::pair<RedisReply, RedisReply>>, ///< Map
        std::monostate                         ///< Null
    >;

    /**
     * @brief Redis 响应值类
     * @details 封装单个 Redis 回复，包含类型标签和数据载荷
     */
    class RedisReply
    {
    public:
        RedisReply();                                      ///< 默认构造（Null 类型）
        RedisReply(RespType type, RespData data);          ///< 从类型和数据构造
        RedisReply(const RedisReply& other);               ///< 拷贝构造
        RedisReply(RedisReply&& other) noexcept;           ///< 移动构造
        RedisReply& operator=(const RedisReply& other);    ///< 拷贝赋值
        RedisReply& operator=(RedisReply&& other) noexcept; ///< 移动赋值

        // 类型判断
        bool isSimpleString() const { return m_type == RespType::SimpleString; } ///< 判断是否为简单字符串
        bool isError() const { return m_type == RespType::Error; }               ///< 判断是否为错误
        bool isInteger() const { return m_type == RespType::Integer; }           ///< 判断是否为整数
        bool isBulkString() const { return m_type == RespType::BulkString; }     ///< 判断是否为批量字符串
        bool isArray() const { return m_type == RespType::Array; }               ///< 判断是否为数组
        bool isNull() const { return m_type == RespType::Null; }                 ///< 判断是否为空值
        bool isDouble() const { return m_type == RespType::Double; }             ///< 判断是否为浮点数
        bool isBoolean() const { return m_type == RespType::Boolean; }           ///< 判断是否为布尔值
        bool isMap() const { return m_type == RespType::Map; }                   ///< 判断是否为映射
        bool isSet() const { return m_type == RespType::Set; }                   ///< 判断是否为集合
        bool isPush() const { return m_type == RespType::Push; }                 ///< 判断是否为推送

        // 获取值
        std::string asString() const;                ///< 转换为字符串
        int64_t asInteger() const;                   ///< 转换为整数
        double asDouble() const;                     ///< 转换为浮点数
        bool asBoolean() const;                      ///< 转换为布尔值
        const std::vector<RedisReply>& asArray() const; ///< 转换为数组引用
        const std::vector<std::pair<RedisReply, RedisReply>>& asMap() const; ///< 转换为映射引用

        RespType getType() const { return m_type; }  ///< 获取类型
        const RespData& getData() const { return m_data; } ///< 获取数据

    private:
        RespType m_type;  ///< 回复类型
        RespData m_data;  ///< 回复数据
    };

    /**
     * @brief Redis 协议解析错误枚举
     */
    enum class ParseError
    {
        Success,        ///< 解析成功
        Incomplete,     ///< 数据不完整，需要更多数据
        InvalidFormat,  ///< 格式错误
        InvalidType,    ///< 无效的类型标识
        InvalidLength,  ///< 无效的长度
        BufferOverflow  ///< 缓冲区溢出
    };

    /**
     * @brief Redis 协议解析器
     * @details 实现 RESP2/RESP3 协议的完整解析功能，
     *          提供常规解析和快速解析两种接口
     */
    class RespParser
    {
    public:
        RespParser();
        ~RespParser();

        /**
         * @brief 解析 RESP 数据
         * @param data 输入数据指针
         * @param length 数据长度
         * @return 解析的字节数和解析结果，或解析错误
         */
        std::expected<std::pair<size_t, RedisReply>, ParseError>
            parse(const char* data, size_t length);

        /**
         * @brief 热路径解析接口，输出到 out，避免 pair 临时对象
         * @param data 输入数据指针
         * @param length 数据长度
         * @param[out] out 解析结果输出
         * @return 解析的字节数，或解析错误
         */
        std::expected<size_t, ParseError> parseFast(const char* data,
                                                    size_t length,
                                                    RedisReply* out);

        /**
         * @brief 重置解析器状态
         */
        void reset();

    private:
        std::expected<size_t, ParseError>
            parseSimpleStringFast(const char* data, size_t length, RedisReply* out); ///< 解析简单字符串 (+OK\r\n)

        std::expected<size_t, ParseError>
            parseErrorFast(const char* data, size_t length, RedisReply* out); ///< 解析错误 (-Error message\r\n)

        std::expected<size_t, ParseError>
            parseIntegerFast(const char* data, size_t length, RedisReply* out); ///< 解析整数 (:1000\r\n)

        std::expected<size_t, ParseError>
            parseBulkStringFast(const char* data, size_t length, RedisReply* out); ///< 解析批量字符串 ($6\r\nfoobar\r\n)

        std::expected<size_t, ParseError>
            parseArrayFast(const char* data, size_t length, RedisReply* out); ///< 解析数组 (*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n)

        std::expected<size_t, ParseError>
            parseDoubleFast(const char* data, size_t length, RedisReply* out); ///< 解析双精度浮点数 (,1.23\r\n) - RESP3

        std::expected<size_t, ParseError>
            parseBooleanFast(const char* data, size_t length, RedisReply* out); ///< 解析布尔值 (#t\r\n or #f\r\n) - RESP3

        std::expected<size_t, ParseError>
            parseMapFast(const char* data, size_t length, RedisReply* out); ///< 解析映射 (%2\r\n...) - RESP3

        std::expected<size_t, ParseError>
            parseSetFast(const char* data, size_t length, RedisReply* out); ///< 解析集合 (~2\r\n...) - RESP3

        /**
         * @brief 查找 CRLF (\r\n) 位置
         * @param data 数据指针
         * @param length 数据长度
         * @param offset 起始偏移
         * @return CRLF 位置，未找到返回空
         */
        std::optional<size_t> findCRLF(const char* data, size_t length, size_t offset = 0);

        /**
         * @brief 解析整数值
         * @param data 数据指针
         * @param length 数据长度
         * @return 解析的整数值，或解析错误
         */
        std::expected<int64_t, ParseError> parseIntegerValue(const char* data, size_t length);
    };

    /**
     * @brief Redis 协议编码器
     * @details 实现 RESP2/RESP3 协议的编码功能，
     *          支持各种类型的编码和命令构建
     */
    class RespEncoder
    {
    public:
        RespEncoder();
        ~RespEncoder();

        /**
         * @brief 编码简单字符串
         * @param str 字符串内容
         * @return 编码后的 RESP 数据
         */
        std::string encodeSimpleString(const std::string& str);

        /**
         * @brief 编码错误消息
         * @param error 错误消息
         * @return 编码后的 RESP 数据
         */
        std::string encodeError(const std::string& error);

        /**
         * @brief 编码整数
         * @param value 整数值
         * @return 编码后的 RESP 数据
         */
        std::string encodeInteger(int64_t value);

        /**
         * @brief 编码批量字符串
         * @param str 字符串内容
         * @return 编码后的 RESP 数据
         */
        std::string encodeBulkString(const std::string& str);

        /**
         * @brief 编码空值
         * @return 编码后的 RESP 数据
         */
        std::string encodeNull();

        /**
         * @brief 编码数组
         * @param elements 数组元素列表
         * @return 编码后的 RESP 数据
         */
        std::string encodeArray(const std::vector<std::string>& elements);

        /**
         * @brief 编码 Redis 命令（模板化版本）
         * @tparam Args 参数类型
         * @param cmd 命令名
         * @param args 命令参数
         * @return 编码后的 RESP 命令
         */
        template<typename... Args>
        std::string encodeCommand(const std::string& cmd, Args&&... args);

        /**
         * @brief 编码 Redis 命令（初始化列表参数）
         * @param cmd 命令名
         * @param args 命令参数初始化列表
         * @return 编码后的 RESP 命令
         */
        std::string encodeCommand(const std::string& cmd, std::initializer_list<std::string> args);

        /**
         * @brief 编码完整的 Redis 命令（容器版本）
         * @tparam Container 容器类型
         * @param cmd_parts 命令各部分
         * @return 编码后的 RESP 命令
         */
        template<typename Container>
        std::string encodeCommand(const Container& cmd_parts);

        /**
         * @brief 编码完整的 Redis 命令（初始化列表版本）
         * @param cmd_parts 命令各部分初始化列表
         * @return 编码后的 RESP 命令
         */
        std::string encodeCommand(std::initializer_list<std::string> cmd_parts);

        /**
         * @brief 追加编码后的命令到输出字符串
         * @param[out] out 输出字符串
         * @param cmd_parts 命令各部分
         */
        void append(std::string& out, const std::vector<std::string>& cmd_parts) const;

        /**
         * @brief 追加编码后的命令到输出字符串
         * @param[out] out 输出字符串
         * @param cmd 命令名
         * @param args 命令参数
         */
        void append(std::string& out,
                           std::string_view cmd,
                           const std::vector<std::string>& args) const;

        /**
         * @brief 追加编码后的命令到输出字符串（span 版本）
         * @param[out] out 输出字符串
         * @param cmd 命令名
         * @param args 命令参数视图
         */
        void append(std::string& out,
                           std::string_view cmd,
                           std::span<const std::string_view> args) const;

        /**
         * @brief 追加编码后的命令到输出字符串（初始化列表版本）
         * @param[out] out 输出字符串
         * @param cmd 命令名
         * @param args 命令参数初始化列表
         */
        void append(std::string& out,
                           std::string_view cmd,
                           std::initializer_list<std::string_view> args) const;

        /**
         * @brief 估算命令编码后的字节数
         * @param cmd 命令名
         * @param args 命令参数
         * @return 预估字节数
         */
        [[nodiscard]] size_t estimateCommandBytes(std::string_view cmd,
                                                  std::span<const std::string_view> args) const
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += estimateBulkStringBytes(cmd.size());
            for (const auto& arg : args) {
                total += estimateBulkStringBytes(arg.size());
            }
            return total;
        }

        [[nodiscard]] size_t estimateCommandBytes(std::string_view cmd,
                                                  std::initializer_list<std::string_view> args) const
        {
            return estimateCommandBytes(
                cmd,
                std::span<const std::string_view>(args.begin(), args.size()));
        }

        [[nodiscard]] size_t estimateCommandBytes(std::string_view cmd,
                                                  const std::vector<std::string>& args) const
        {
            size_t total = 1 + decimalDigits(1 + args.size()) + 2;
            total += estimateBulkStringBytes(cmd.size());
            for (const auto& arg : args) {
                total += estimateBulkStringBytes(arg.size());
            }
            return total;
        }

        /**
         * @brief 快速路径：追加命令（调用方必须确保输出缓冲区已预留足够空间）
         * @param[out] out 输出字符串
         * @param cmd_parts 命令各部分
         */
        void appendCommandFast(std::string& out, const std::vector<std::string>& cmd_parts) const
        {
            if (cmd_parts.empty()) {
                out += "*0\r\n";
                return;
            }
            out.push_back('*');
            appendUnsignedDecimal(out, cmd_parts.size());
            out += "\r\n";
            for (const auto& part : cmd_parts) {
                appendBulkString(out, part);
            }
        }

        /**
         * @brief 快速路径：追加命令（span 参数版本）
         */
        void appendCommandFast(std::string& out,
                               std::string_view cmd,
                               std::span<const std::string_view> args) const
        {
            const size_t arg_count = 1 + args.size();
            out.push_back('*');
            appendUnsignedDecimal(out, arg_count);
            out += "\r\n";
            appendBulkString(out, cmd);
            for (const auto& arg : args) {
                appendBulkString(out, arg);
            }
        }

        /**
         * @brief 快速路径：追加命令（vector 参数版本）
         */
        void appendCommandFast(std::string& out,
                               std::string_view cmd,
                               const std::vector<std::string>& args) const
        {
            const size_t arg_count = 1 + args.size();
            out.push_back('*');
            appendUnsignedDecimal(out, arg_count);
            out += "\r\n";
            appendBulkString(out, cmd);
            for (const auto& arg : args) {
                appendBulkString(out, arg);
            }
        }

        /**
         * @brief 快速路径：追加命令（初始化列表版本）
         */
        void appendCommandFast(std::string& out,
                               std::string_view cmd,
                               std::initializer_list<std::string_view> args) const
        {
            const size_t arg_count = 1 + args.size();
            out.push_back('*');
            appendUnsignedDecimal(out, arg_count);
            out += "\r\n";
            appendBulkString(out, cmd);
            for (const auto& arg : args) {
                appendBulkString(out, arg);
            }
        }

    private:
        /**
         * @brief 追加无符号十进制数字符串到输出
         * @param[out] out 输出字符串
         * @param value 无符号整数值
         */
        static void appendUnsignedDecimal(std::string& out, size_t value)
        {
            char buf[32];
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec == std::errc()) {
                out.append(buf, static_cast<size_t>(ptr - buf));
            } else {
                out += std::to_string(value);
            }
        }

        /**
         * @brief 计算无符号整数的十进制位数
         * @param value 无符号整数值
         * @return 十进制位数
         */
        static size_t decimalDigits(size_t value)
        {
            size_t digits = 1;
            while (value >= 10) {
                value /= 10;
                ++digits;
            }
            return digits;
        }

        /**
         * @brief 估算批量字符串编码后的字节数
         * @param value_len 原始值长度
         * @return 编码后的预估字节数
         */
        static size_t estimateBulkStringBytes(size_t value_len)
        {
            return 1 + decimalDigits(value_len) + 2 + value_len + 2;
        }

        /**
         * @brief 追加编码后的批量字符串到输出
         * @param[out] out 输出字符串
         * @param value 字符串值
         */
        static void appendBulkString(std::string& out, std::string_view value)
        {
            out.push_back('$');
            appendUnsignedDecimal(out, value.size());
            out += "\r\n";
            out.append(value.data(), value.size());
            out += "\r\n";
        }

        /**
         * @brief 追加命令部分（自动类型推导和编码）
         * @tparam T 值类型
         * @param[out] out 输出字符串
         * @param value 命令部分值
         */
        template<typename T>
        static void appendCommandPart(std::string& out, T&& value)
        {
            using Decayed = std::decay_t<T>;
            if constexpr (std::is_same_v<Decayed, std::string>) {
                appendBulkString(out, value);
            } else if constexpr (std::is_same_v<Decayed, std::string_view>) {
                appendBulkString(out, value);
            } else if constexpr (std::is_same_v<Decayed, const char*> ||
                                 std::is_same_v<Decayed, char*>) {
                appendBulkString(out, value ? std::string_view(value) : std::string_view{});
            } else if constexpr (std::is_integral_v<Decayed>) {
                char buf[32];
                auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
                if (ec == std::errc()) {
                    appendBulkString(out, std::string_view(buf, static_cast<size_t>(ptr - buf)));
                } else {
                    auto str = std::to_string(value);
                    appendBulkString(out, str);
                }
            } else if constexpr (std::is_floating_point_v<Decayed>) {
                auto str = std::to_string(value);
                appendBulkString(out, str);
            } else if constexpr (std::is_convertible_v<T, std::string_view>) {
                appendBulkString(out, std::string_view(value));
            } else {
                auto str = std::string(value);
                appendBulkString(out, str);
            }
        }

        /**
         * @brief 将参数转换为字符串（辅助函数）
         * @tparam T 值类型
         * @param value 输入值
         * @return 字符串表示
         */
        template<typename T>
        std::string toString(T&& value);

        /**
         * @brief 递归构建命令参数（辅助函数）
         * @tparam T 第一个参数类型
         * @tparam Rest 剩余参数类型
         * @param[out] result 输出字符串
         * @param first 第一个参数
         * @param rest 剩余参数
         */
        template<typename T, typename... Rest>
        void buildCommandArgs(std::string& result, T&& first, Rest&&... rest);

        // RESP3扩展
        std::string encodeDouble(double value);   ///< 编码双精度浮点数（RESP3）
        std::string encodeBoolean(bool value);     ///< 编码布尔值（RESP3）
    };

    // 模板实现必须在头文件中
    template<typename T>
    std::string RespEncoder::toString(T&& value)
    {
        if constexpr (std::is_same_v<std::decay_t<T>, std::string> ||
                      std::is_same_v<std::decay_t<T>, const char*> ||
                      std::is_convertible_v<T, std::string_view>) {
            return std::string(std::forward<T>(value));
        } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
            return std::to_string(value);
        } else if constexpr (std::is_floating_point_v<std::decay_t<T>>) {
            return std::to_string(value);
        } else {
            // 对于其他类型，尝试转换为字符串
            return std::string(value);
        }
    }

    template<typename T, typename... Rest>
    void RespEncoder::buildCommandArgs(std::string& result, T&& first, Rest&&... rest)
    {
        appendCommandPart(result, std::forward<T>(first));
        if constexpr (sizeof...(rest) > 0) {
            buildCommandArgs(result, std::forward<Rest>(rest)...);
        }
    }

    template<typename... Args>
    std::string RespEncoder::encodeCommand(const std::string& cmd, Args&&... args)
    {
        const size_t arg_count = 1 + sizeof...(args);
        std::string result;
        result.reserve(1 + decimalDigits(arg_count) + 2 + estimateBulkStringBytes(cmd.size()));
        result.push_back('*');
        result += std::to_string(arg_count);
        result += "\r\n";
        appendBulkString(result, cmd);
        if constexpr (sizeof...(args) > 0) {
            buildCommandArgs(result, std::forward<Args>(args)...);
        }
        return result;
    }

    template<typename Container>
    std::string RespEncoder::encodeCommand(const Container& cmd_parts)
    {
        size_t size = 0;
        if constexpr (requires { cmd_parts.size(); }) {
            size = cmd_parts.size();
        } else {
            size = std::distance(std::begin(cmd_parts), std::end(cmd_parts));
        }

        if (size == 0) {
            return "*0\r\n";
        }

        std::string result;
        result.reserve(1 + decimalDigits(size) + 2 + size * 8);
        result.push_back('*');
        result += std::to_string(size);
        result += "\r\n";
        for (const auto& part : cmd_parts) {
            appendCommandPart(result, part);
        }
        return result;
    }

} // namespace galay::redis::protocol

#endif // GALAY_REDIS_PROTOCOL_H
