/**
 * @file mysql_value.h
 * @brief MySQL字段类型、字段定义、行数据和结果集
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 定义了MySQL协议中的字段类型枚举、字段标志、列定义(MysqlField)、
 *          行数据(MysqlRow)和完整结果集(MysqlResultSet)。
 */

#ifndef GALAY_MYSQL_VALUE_H
#define GALAY_MYSQL_VALUE_H

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <unordered_map>

namespace galay::mysql
{

/**
 * @brief MySQL字段类型枚举
 * @details 对应MySQL协议中的MySQLType值
 */
enum class MysqlFieldType : uint8_t
{
    DECIMAL     = 0x00,  ///< DECIMAL类型
    TINY        = 0x01,  ///< TINYINT类型
    SHORT       = 0x02,  ///< SMALLINT类型
    LONG        = 0x03,  ///< INT类型
    FLOAT       = 0x04,  ///< FLOAT类型
    DOUBLE      = 0x05,  ///< DOUBLE类型
    NULL_TYPE   = 0x06,  ///< NULL类型
    TIMESTAMP   = 0x07,  ///< TIMESTAMP类型
    LONGLONG    = 0x08,  ///< BIGINT类型
    INT24       = 0x09,  ///< MEDIUMINT类型
    DATE        = 0x0a,  ///< DATE类型
    TIME        = 0x0b,  ///< TIME类型
    DATETIME    = 0x0c,  ///< DATETIME类型
    YEAR        = 0x0d,  ///< YEAR类型
    NEWDATE     = 0x0e,  ///< NEWDATE类型
    VARCHAR     = 0x0f,  ///< VARCHAR类型
    BIT         = 0x10,  ///< BIT类型
    JSON        = 0xf5,  ///< JSON类型
    NEWDECIMAL  = 0xf6,  ///< NEWDECIMAL类型
    ENUM        = 0xf7,  ///< ENUM类型
    SET         = 0xf8,  ///< SET类型
    TINY_BLOB   = 0xf9,  ///< TINYBLOB类型
    MEDIUM_BLOB = 0xfa,  ///< MEDIUMBLOB类型
    LONG_BLOB   = 0xfb,  ///< LONGBLOB类型
    BLOB        = 0xfc,  ///< BLOB类型
    VAR_STRING  = 0xfd,  ///< VAR_STRING类型
    STRING      = 0xfe,  ///< STRING类型
    GEOMETRY    = 0xff,  ///< GEOMETRY类型
};

/**
 * @brief MySQL字段标志位
 * @details 对应MySQL协议中ColumnDefinition的flags字段
 */
enum MysqlFieldFlags : uint16_t
{
    NOT_NULL_FLAG       = 0x0001, ///< 不允许NULL值
    PRI_KEY_FLAG        = 0x0002, ///< 主键字段
    UNIQUE_KEY_FLAG     = 0x0004, ///< 唯一键字段
    MULTIPLE_KEY_FLAG   = 0x0008, ///< 非唯一键字段
    BLOB_FLAG           = 0x0010, ///< BLOB字段
    UNSIGNED_FLAG       = 0x0020, ///< 无符号字段
    ZEROFILL_FLAG       = 0x0040, ///< 零填充字段
    BINARY_FLAG         = 0x0080, ///< 二进制字段
    ENUM_FLAG           = 0x0100, ///< ENUM字段
    AUTO_INCREMENT_FLAG = 0x0200, ///< 自增字段
    TIMESTAMP_FLAG      = 0x0400, ///< TIMESTAMP字段
    SET_FLAG            = 0x0800, ///< SET字段
    NUM_FLAG            = 0x8000, ///< 数值字段
};

/**
 * @brief 列定义
 * @details 表示MySQL结果集中一列的元数据信息，包括列名、类型、标志等。
 */
class MysqlField
{
public:
    MysqlField() = default;

    /**
     * @brief 构造列定义
     * @param name 列名
     * @param type 字段类型
     * @param flags 字段标志
     * @param column_length 列长度
     * @param decimals 小数位数
     */
    MysqlField(std::string name, MysqlFieldType type, uint16_t flags,
               uint32_t column_length, uint8_t decimals);

    const std::string& name() const { return m_name; }             ///< 获取列名
    MysqlFieldType type() const { return m_type; }                 ///< 获取字段类型
    uint16_t flags() const { return m_flags; }                     ///< 获取字段标志
    uint32_t columnLength() const { return m_column_length; }      ///< 获取列长度
    uint8_t decimals() const { return m_decimals; }                ///< 获取小数位数

    void setCatalog(std::string catalog) { m_catalog = std::move(catalog); }     ///< 设置目录名
    void setSchema(std::string schema) { m_schema = std::move(schema); }         ///< 设置数据库名
    void setTable(std::string table) { m_table = std::move(table); }             ///< 设置表名
    void setOrgTable(std::string org_table) { m_org_table = std::move(org_table); } ///< 设置原始表名
    void setOrgName(std::string org_name) { m_org_name = std::move(org_name); }  ///< 设置原始列名
    void setCharacterSet(uint16_t cs) { m_character_set = cs; }                  ///< 设置字符集

    const std::string& catalog() const { return m_catalog; }       ///< 获取目录名
    const std::string& schema() const { return m_schema; }         ///< 获取数据库名
    const std::string& table() const { return m_table; }           ///< 获取表名
    const std::string& orgTable() const { return m_org_table; }    ///< 获取原始表名
    const std::string& orgName() const { return m_org_name; }      ///< 获取原始列名
    uint16_t characterSet() const { return m_character_set; }      ///< 获取字符集

    bool isNotNull() const { return m_flags & NOT_NULL_FLAG; }         ///< 是否不允许NULL
    bool isPrimaryKey() const { return m_flags & PRI_KEY_FLAG; }       ///< 是否为主键
    bool isAutoIncrement() const { return m_flags & AUTO_INCREMENT_FLAG; } ///< 是否自增
    bool isUnsigned() const { return m_flags & UNSIGNED_FLAG; }        ///< 是否无符号

private:
    std::string m_catalog;                          ///< 目录名
    std::string m_schema;                           ///< 数据库名
    std::string m_table;                            ///< 表名
    std::string m_org_table;                        ///< 原始表名
    std::string m_name;                             ///< 列名
    std::string m_org_name;                         ///< 原始列名
    uint32_t m_column_length = 0;                   ///< 列长度
    uint16_t m_character_set = 0;                   ///< 字符集编码
    uint16_t m_flags = 0;                           ///< 字段标志位
    MysqlFieldType m_type = MysqlFieldType::NULL_TYPE; ///< 字段类型
    uint8_t m_decimals = 0;                         ///< 小数位数
};

/**
 * @brief 单行数据
 * @details 表示MySQL结果集中的一行，每列值以optional<string>形式存储，
 *          nullopt表示该列为NULL。
 */
class MysqlRow
{
public:
    MysqlRow() = default;

    /**
     * @brief 构造指定列值的行
     * @param values 列值数组
     */
    explicit MysqlRow(std::vector<std::optional<std::string>> values);

    size_t size() const { return m_values.size(); }  ///< 获取列数
    bool empty() const { return m_values.empty(); }   ///< 判断是否为空行

    /**
     * @brief 通过索引访问列值
     * @param index 列索引
     * @return 列值，NULL时为nullopt
     */
    const std::optional<std::string>& operator[](size_t index) const;

    /**
     * @brief 通过索引访问列值（带边界检查）
     * @param index 列索引
     * @return 列值，NULL时为nullopt
     */
    const std::optional<std::string>& at(size_t index) const;

    /**
     * @brief 判断指定列是否为NULL
     * @param index 列索引
     * @return 为NULL时返回true
     */
    bool isNull(size_t index) const;

    /**
     * @brief 获取指定列的字符串值
     * @param index 列索引
     * @param default_val 列为NULL时的默认值
     * @return 字符串值
     */
    std::string getString(size_t index, const std::string& default_val = "") const;

    /**
     * @brief 获取指定列的有符号64位整数值
     * @param index 列索引
     * @param default_val 列为NULL或转换失败时的默认值
     * @return 整数值
     */
    int64_t getInt64(size_t index, int64_t default_val = 0) const;

    /**
     * @brief 获取指定列的无符号64位整数值
     * @param index 列索引
     * @param default_val 列为NULL或转换失败时的默认值
     * @return 整数值
     */
    uint64_t getUint64(size_t index, uint64_t default_val = 0) const;

    /**
     * @brief 获取指定列的双精度浮点值
     * @param index 列索引
     * @param default_val 列为NULL或转换失败时的默认值
     * @return 浮点值
     */
    double getDouble(size_t index, double default_val = 0.0) const;

    /**
     * @brief 获取所有列值的引用
     * @return 列值数组的常量引用
     */
    const std::vector<std::optional<std::string>>& values() const { return m_values; }

private:
    std::vector<std::optional<std::string>> m_values; ///< 列值数组
};

/**
 * @brief 完整结果集
 * @details 包含查询结果的列定义、行数据以及OK包信息（影响行数、最后插入ID等）。
 *          如果没有列定义（hasResultSet()为false），则表示仅包含OK包的响应。
 */
class MysqlResultSet
{
public:
    MysqlResultSet() = default;

    /**
     * @brief 添加列定义
     * @param field 列定义
     */
    void addField(MysqlField field);

    /**
     * @brief 预分配列定义空间
     * @param n 预分配数量
     */
    void reserveFields(size_t n) { m_fields.reserve(n); }

    /**
     * @brief 获取列数
     * @return 列数量
     */
    size_t fieldCount() const { return m_fields.size(); }

    /**
     * @brief 获取指定索引的列定义
     * @param index 列索引
     * @return 列定义引用
     */
    const MysqlField& field(size_t index) const;

    /**
     * @brief 获取所有列定义
     * @return 列定义数组的常量引用
     */
    const std::vector<MysqlField>& fields() const { return m_fields; }

    /**
     * @brief 添加行数据
     * @param row 行数据
     */
    void addRow(MysqlRow row);

    /**
     * @brief 预分配行数据空间
     * @param n 预分配数量
     */
    void reserveRows(size_t n) { m_rows.reserve(n); }

    /**
     * @brief 获取行数
     * @return 行数量
     */
    size_t rowCount() const { return m_rows.size(); }

    /**
     * @brief 获取指定索引的行数据
     * @param index 行索引
     * @return 行数据引用
     */
    const MysqlRow& row(size_t index) const;

    /**
     * @brief 获取所有行数据
     * @return 行数据数组的常量引用
     */
    const std::vector<MysqlRow>& rows() const { return m_rows; }

    /**
     * @brief 按列名查找列索引
     * @param name 列名
     * @return 列索引，未找到返回-1
     */
    int findField(const std::string& name) const;

    // OK包信息
    void setAffectedRows(uint64_t n) { m_affected_rows = n; }     ///< 设置影响行数
    void setLastInsertId(uint64_t id) { m_last_insert_id = id; }  ///< 设置最后插入ID
    void setWarnings(uint16_t w) { m_warnings = w; }              ///< 设置警告数
    void setStatusFlags(uint16_t f) { m_status_flags = f; }       ///< 设置状态标志
    void setInfo(std::string info) { m_info = std::move(info); }  ///< 设置附加信息

    uint64_t affectedRows() const { return m_affected_rows; }     ///< 获取影响行数
    uint64_t lastInsertId() const { return m_last_insert_id; }    ///< 获取最后插入ID
    uint16_t warnings() const { return m_warnings; }              ///< 获取警告数
    uint16_t statusFlags() const { return m_status_flags; }       ///< 获取状态标志
    const std::string& info() const { return m_info; }            ///< 获取附加信息

    /**
     * @brief 判断是否包含结果集（有列定义）
     * @return 有列定义时返回true，仅OK包时返回false
     */
    bool hasResultSet() const { return !m_fields.empty(); }

private:
    std::vector<MysqlField> m_fields;      ///< 列定义数组
    std::vector<MysqlRow> m_rows;          ///< 行数据数组
    std::string m_info;                    ///< 附加信息
    uint64_t m_affected_rows = 0;          ///< 影响行数
    uint64_t m_last_insert_id = 0;         ///< 最后插入ID
    uint16_t m_warnings = 0;               ///< 警告数
    uint16_t m_status_flags = 0;           ///< 状态标志
};

} // namespace galay::mysql

#endif // GALAY_MYSQL_VALUE_H
