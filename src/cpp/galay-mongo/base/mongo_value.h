/**
 * @file mongo_value.h
 * @brief BSON 值类型、文档与数组的 C++ 表示
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 定义 MongoDB BSON 数据模型的 C++ 抽象，包括：
 * - MongoValueType 枚举：BSON 值类型标识
 * - MongoValue 类：BSON 值的通用容器，支持隐式类型转换
 * - MongoArray 类：有序的 BSON 数组
 * - MongoDocument 类：保持字段插入顺序的键值对集合
 * - MongoReply 类：服务端响应的封装
 */

#ifndef GALAY_MONGO_VALUE_H
#define GALAY_MONGO_VALUE_H

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace galay::mongo
{

class MongoDocument;
class MongoArray;

/**
 * @brief BSON 值类型枚举
 */
enum class MongoValueType : uint8_t
{
    Null,       ///< 空值
    Bool,       ///< 布尔值
    Int32,      ///< 32 位整数
    Int64,      ///< 64 位整数
    Double,     ///< 双精度浮点数
    String,     ///< UTF-8 字符串
    Binary,     ///< 二进制数据
    Document,   ///< 嵌套文档
    Array,      ///< 数组
    ObjectId,   ///< 12 字节 ObjectId
    DateTime,   ///< UTC 日期时间（毫秒时间戳）
    Timestamp,  ///< MongoDB 内部时间戳
};

/**
 * @brief BSON 值的通用容器
 * @details 支持 Null/Bool/Int32/Int64/Double/String/Binary/Document/Array，
 * 通过隐式构造函数可直接从 C++ 原生类型转换。
 * 使用 std::variant 存储实际值，通过类型标签区分特殊类型（ObjectId、DateTime、Timestamp）。
 */
class MongoValue
{
public:
    using Binary = std::vector<uint8_t>;  ///< 二进制数据类型

    MongoValue();                       ///< 构造 Null 值
    MongoValue(std::nullptr_t);         ///< 构造 Null 值
    MongoValue(bool value);             ///< 构造 Bool 值
    MongoValue(int32_t value);          ///< 构造 Int32 值
    MongoValue(int64_t value);          ///< 构造 Int64 值
    MongoValue(double value);           ///< 构造 Double 值
    MongoValue(std::string value);      ///< 构造 String 值
    MongoValue(const char* value);      ///< 构造 String 值（从 C 字符串）
    MongoValue(Binary value);           ///< 构造 Binary 值
    MongoValue(MongoDocument value);    ///< 构造嵌套 Document 值
    MongoValue(MongoArray value);       ///< 构造 Array 值
    MongoValue(MongoValue&&) noexcept = default;             ///< 移动构造，转移 BSON 值所有权
    MongoValue& operator=(MongoValue&&) noexcept = default;  ///< 移动赋值，转移 BSON 值所有权

    /**
     * @brief 显式深拷贝 BSON 值
     * @return 独立副本；嵌套 Document/Array 会递归 clone，不共享可变状态
     */
    [[nodiscard]] MongoValue clone() const;

    /// @name 工厂方法（用于 BSON 特殊类型，保留类型信息）
    /// @{
    /**
     * @brief 从十六进制字符串创建 ObjectId 值
     * @param oid 24 字符的十六进制 ObjectId 字符串
     * @return 成功时返回类型为 ObjectId 的 MongoValue；失败时返回错误描述
     */
    static std::expected<MongoValue, std::string> fromObjectId(std::string oid);

    /**
     * @brief 从毫秒时间戳创建 DateTime 值
     * @param millis UTC 毫秒时间戳
     * @return 类型为 DateTime 的 MongoValue
     */
    static MongoValue fromDateTime(int64_t millis);

    /**
     * @brief 从时间戳创建 Timestamp 值
     * @param ts MongoDB 内部时间戳
     * @return 类型为 Timestamp 的 MongoValue
     */
    static MongoValue fromTimestamp(uint64_t ts);
    /// @}

    /**
     * @brief 返回当前值的类型
     * @return BSON 值类型枚举
     */
    MongoValueType type() const;

    /// @name 类型判断
    /// @{
    bool isNull() const;       ///< 判断是否为 Null
    bool isBool() const;       ///< 判断是否为 Bool
    bool isInt32() const;      ///< 判断是否为 Int32
    bool isInt64() const;      ///< 判断是否为 Int64
    bool isDouble() const;     ///< 判断是否为 Double
    bool isString() const;     ///< 判断是否为 String
    bool isBinary() const;     ///< 判断是否为 Binary
    bool isDocument() const;   ///< 判断是否为 Document
    bool isArray() const;      ///< 判断是否为 Array
    bool isObjectId() const;   ///< 判断是否为 ObjectId
    bool isDateTime() const;   ///< 判断是否为 DateTime
    bool isTimestamp() const;  ///< 判断是否为 Timestamp
    /// @}

    /// @name 值提取（类型不匹配时返回默认值或空引用）
    /// @{
    /**
     * @brief 提取布尔值
     * @param default_value 类型不匹配时的默认值
     * @return 布尔值
     */
    bool toBool(bool default_value = false) const;

    /**
     * @brief 提取 32 位整数值
     * @param default_value 类型不匹配时的默认值
     * @return 32 位整数
     */
    int32_t toInt32(int32_t default_value = 0) const;

    /**
     * @brief 提取 64 位整数值
     * @param default_value 类型不匹配时的默认值
     * @return 64 位整数
     */
    int64_t toInt64(int64_t default_value = 0) const;

    /**
     * @brief 提取双精度浮点数值
     * @param default_value 类型不匹配时的默认值
     * @return 双精度浮点数
     */
    double toDouble(double default_value = 0.0) const;

    /**
     * @brief 提取字符串引用
     * @return 字符串的只读引用；类型不匹配时返回空字符串引用
     */
    const std::string& toString() const;

    /**
     * @brief 提取二进制数据引用
     * @return 二进制数据的只读引用；类型不匹配时返回空 Binary 引用
     */
    const Binary& toBinary() const;

    /**
     * @brief 提取嵌套文档引用
     * @return 文档的只读引用
     */
    const MongoDocument& toDocument() const;

    /**
     * @brief 提取数组引用
     * @return 数组的只读引用
     */
    const MongoArray& toArray() const;
    /// @}

    /// @name 可变引用访问（类型不匹配时行为未定义）
    /// @{
    MongoDocument& asDocument();  ///< 获取文档的可变引用
    MongoArray& asArray();        ///< 获取数组的可变引用
    /// @}

private:
    MongoValue(const MongoValue&) = delete;
    MongoValue& operator=(const MongoValue&) = delete;

    using DocumentPtr = std::shared_ptr<MongoDocument>;  ///< 文档共享指针类型
    using ArrayPtr = std::shared_ptr<MongoArray>;        ///< 数组共享指针类型
    using Storage = std::variant<std::nullptr_t,
                                 bool,
                                 int32_t,
                                 int64_t,
                                 double,
                                 std::string,
                                 Binary,
                                 DocumentPtr,
                                 ArrayPtr>;

    Storage m_storage;                              ///< 值存储
    MongoValueType m_type_tag = MongoValueType::Null; ///< 类型标签

    struct ObjectIdTag {};    ///< ObjectId 构造标签
    struct DateTimeTag {};    ///< DateTime 构造标签
    struct TimestampTag {};   ///< Timestamp 构造标签
    MongoValue(ObjectIdTag, std::string oid);    ///< ObjectId 内部构造
    MongoValue(DateTimeTag, int64_t millis);     ///< DateTime 内部构造
    MongoValue(TimestampTag, uint64_t ts);       ///< Timestamp 内部构造

    static const std::string kEmptyString;   ///< 空字符串常量（类型不匹配时返回）
    static const Binary kEmptyBinary;        ///< 空 Binary 常量（类型不匹配时返回）
};

/**
 * @brief BSON 数组，有序的 MongoValue 集合
 * @details 按索引顺序存储元素，支持追加、随机访问和容量预留
 */
class MongoArray
{
public:
    MongoArray() = default;
    MongoArray(MongoArray&&) noexcept = default;             ///< 移动构造，转移数组元素所有权
    MongoArray& operator=(MongoArray&&) noexcept = default;  ///< 移动赋值，转移数组元素所有权

    /**
     * @brief 从已有值列表构造
     * @param values 值列表
     */
    explicit MongoArray(std::vector<MongoValue> values);

    /**
     * @brief 显式深拷贝数组
     * @return 元素递归 clone 后的新数组
     */
    [[nodiscard]] MongoArray clone() const;

    /**
     * @brief 追加一个元素
     * @param value 要追加的值
     */
    void append(MongoValue value);

    /**
     * @brief 预分配容量
     * @param n 预分配的元素数量
     */
    void reserve(size_t n);

    /**
     * @brief 返回元素数量
     * @return 数组长度
     */
    size_t size() const;

    /**
     * @brief 判断是否为空
     * @return 数组为空时返回 true
     */
    bool empty() const;

    /**
     * @brief 按索引访问（带边界检查）
     * @param index 元素索引
     * @return 成功时返回元素的只读引用；越界时返回错误描述
     */
    std::expected<std::reference_wrapper<const MongoValue>, std::string> at(size_t index) const;

    /**
     * @brief 按索引访问（不做边界检查）
     * @param index 元素索引
     * @return 元素的只读引用
     */
    const MongoValue& operator[](size_t index) const;

    /**
     * @brief 获取底层值列表的只读引用
     * @return 值列表的只读引用
     */
    const std::vector<MongoValue>& values() const;

    /**
     * @brief 获取底层值列表的可变引用
     * @return 值列表的可变引用
     */
    std::vector<MongoValue>& values();

private:
    MongoArray(const MongoArray&) = delete;
    MongoArray& operator=(const MongoArray&) = delete;

    std::vector<MongoValue> m_values;  ///< 底层值存储
};

/**
 * @brief BSON 文档，保持字段插入顺序的键值对集合
 * @details 字段按插入顺序存储，支持追加、设置、查找和便捷取值方法。
 * 不对键的唯一性做强制约束，append 不检查重复键，set 方法会更新已存在的键。
 */
class MongoDocument
{
public:
    using Field = std::pair<std::string, MongoValue>;  ///< 字段类型（键值对）

    MongoDocument() = default;
    MongoDocument(MongoDocument&&) noexcept = default;             ///< 移动构造，转移字段所有权
    MongoDocument& operator=(MongoDocument&&) noexcept = default;  ///< 移动赋值，转移字段所有权

    /**
     * @brief 从已有字段列表构造
     * @param fields 字段列表
     */
    explicit MongoDocument(std::vector<Field> fields);

    /**
     * @brief 显式深拷贝文档
     * @return 字段值递归 clone 后的新文档
     */
    [[nodiscard]] MongoDocument clone() const;

    /**
     * @brief 追加字段（不检查重复键）
     * @param key 字段名
     * @param value 字段值
     */
    void append(std::string key, MongoValue value);

    /**
     * @brief 设置字段（已存在则更新，否则追加）
     * @param key 字段名
     * @param value 字段值
     */
    void set(std::string key, MongoValue value);

    /**
     * @brief 判断是否存在指定键
     * @param key 字段名
     * @return 存在时返回 true
     */
    bool has(const std::string& key) const;

    /**
     * @brief 查找指定键，未找到返回 nullptr
     * @param key 字段名
     * @return 值指针；未找到时为 nullptr
     */
    const MongoValue* find(const std::string& key) const;

    /**
     * @brief 查找指定键（可变版本），未找到返回 nullptr
     * @param key 字段名
     * @return 可变值指针；未找到时为 nullptr
     */
    MongoValue* find(const std::string& key);

    /**
     * @brief 按键访问
     * @param key 字段名
     * @return 成功时返回值的只读引用；键不存在时返回错误描述
     */
    std::expected<std::reference_wrapper<const MongoValue>, std::string> at(const std::string& key) const;

    /// @name 便捷取值方法（键不存在或类型不匹配时返回默认值）
    /// @{
    std::string getString(const std::string& key, std::string default_value = "") const;     ///< 获取字符串值
    int32_t getInt32(const std::string& key, int32_t default_value = 0) const;               ///< 获取 32 位整数值
    int64_t getInt64(const std::string& key, int64_t default_value = 0) const;               ///< 获取 64 位整数值
    double getDouble(const std::string& key, double default_value = 0.0) const;              ///< 获取双精度浮点数值
    bool getBool(const std::string& key, bool default_value = false) const;                  ///< 获取布尔值
    /// @}

    /**
     * @brief 返回字段数量
     * @return 字段数
     */
    size_t size() const;

    /**
     * @brief 判断是否为空
     * @return 无字段时返回 true
     */
    bool empty() const;

    /**
     * @brief 获取底层字段列表的只读引用
     * @return 字段列表的只读引用
     */
    const std::vector<Field>& fields() const;

    /**
     * @brief 获取底层字段列表的可变引用
     * @return 字段列表的可变引用
     */
    std::vector<Field>& fields();

private:
    MongoDocument(const MongoDocument&) = delete;
    MongoDocument& operator=(const MongoDocument&) = delete;

    std::vector<Field> m_fields;  ///< 底层字段存储
};

/**
 * @brief MongoDB 服务端响应的封装，提供 ok/error 状态判断
 * @details 封装原始响应文档，提供便捷方法判断响应状态和提取错误信息
 */
class MongoReply
{
public:
    MongoReply() = default;
    MongoReply(MongoReply&&) noexcept = default;             ///< 移动构造，转移响应文档所有权
    MongoReply& operator=(MongoReply&&) noexcept = default;  ///< 移动赋值，转移响应文档所有权

    /**
     * @brief 从原始响应文档构造
     * @param document 服务端响应文档
     */
    explicit MongoReply(MongoDocument document);

    /**
     * @brief 显式深拷贝响应
     * @return 响应文档 clone 后的新响应
     */
    [[nodiscard]] MongoReply clone() const;

    /**
     * @brief 获取原始响应文档
     * @return 文档的只读引用
     */
    const MongoDocument& document() const;

    /**
     * @brief 获取原始响应文档（可变）
     * @return 文档的可变引用
     */
    MongoDocument& document();

    /**
     * @brief 判断响应是否成功（ok == 1）
     * @return 成功时返回 true
     */
    bool ok() const;

    /**
     * @brief 判断响应中是否包含命令错误
     * @return 包含错误时返回 true
     */
    bool hasCommandError() const;

    /**
     * @brief 返回服务端错误码
     * @return 错误码；无错误时返回 0
     */
    int32_t errorCode() const;

    /**
     * @brief 返回服务端错误消息
     * @return 错误消息字符串
     */
    std::string errorMessage() const;

private:
    MongoReply(const MongoReply&) = delete;
    MongoReply& operator=(const MongoReply&) = delete;

    MongoDocument m_document;  ///< 原始响应文档
};

} // namespace galay::mongo

#endif // GALAY_MONGO_VALUE_H
