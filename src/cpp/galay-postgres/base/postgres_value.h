/**
 * @file postgres_value.h
 * @brief PostgreSQL column metadata, rows, and result sets.
 */

#ifndef GALAY_POSTGRES_VALUE_H
#define GALAY_POSTGRES_VALUE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::postgres
{

enum class PostgresOid : uint32_t
{
    BOOL = 16,
    BYTEA = 17,
    CHAR = 18,
    INT8 = 20,
    INT2 = 21,
    INT4 = 23,
    TEXT = 25,
    OID = 26,
    JSON = 114,
    FLOAT4 = 700,
    FLOAT8 = 701,
    VARCHAR = 1043,
    DATE = 1082,
    TIME = 1083,
    TIMESTAMP = 1114,
    TIMESTAMPTZ = 1184,
    NUMERIC = 1700,
    UUID = 2950,
    JSONB = 3802,
};

class PostgresField
{
public:
    PostgresField() = default;
    PostgresField(std::string name,
                  uint32_t table_oid,
                  int16_t column_index,
                  uint32_t type_oid,
                  int16_t type_size,
                  int32_t type_modifier,
                  int16_t format);
    PostgresField(PostgresField&&) noexcept = default;
    PostgresField& operator=(PostgresField&&) noexcept = default;

    [[nodiscard]] PostgresField clone() const;
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] uint32_t tableOid() const noexcept { return m_table_oid; }
    [[nodiscard]] uint32_t typeOid() const noexcept { return m_type_oid; }
    [[nodiscard]] int32_t typeModifier() const noexcept { return m_type_modifier; }
    [[nodiscard]] int16_t columnIndex() const noexcept { return m_column_index; }
    [[nodiscard]] int16_t typeSize() const noexcept { return m_type_size; }
    [[nodiscard]] int16_t format() const noexcept { return m_format; }

private:
    std::string m_name;
    uint32_t m_table_oid = 0;
    uint32_t m_type_oid = 0;
    int32_t m_type_modifier = -1;
    int16_t m_column_index = 0;
    int16_t m_type_size = -1;
    int16_t m_format = 0;

    PostgresField(const PostgresField&) = delete;
    PostgresField& operator=(const PostgresField&) = delete;
};

class PostgresRow
{
public:
    PostgresRow() = default;
    explicit PostgresRow(std::vector<std::optional<std::string>> values);
    PostgresRow(PostgresRow&&) noexcept = default;
    PostgresRow& operator=(PostgresRow&&) noexcept = default;

    [[nodiscard]] PostgresRow clone() const;
    [[nodiscard]] size_t size() const noexcept { return m_values.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_values.empty(); }
    [[nodiscard]] const std::optional<std::string>& operator[](size_t index) const noexcept;
    [[nodiscard]] bool isNull(size_t index) const noexcept;
    [[nodiscard]] std::string getString(size_t index,
                                        const std::string& default_value = "") const;
    [[nodiscard]] int64_t getInt64(size_t index, int64_t default_value = 0) const noexcept;
    [[nodiscard]] uint64_t getUint64(size_t index, uint64_t default_value = 0) const noexcept;
    [[nodiscard]] double getDouble(size_t index, double default_value = 0.0) const noexcept;
    [[nodiscard]] const std::vector<std::optional<std::string>>& values() const noexcept
    {
        return m_values;
    }

private:
    std::vector<std::optional<std::string>> m_values;

    PostgresRow(const PostgresRow&) = delete;
    PostgresRow& operator=(const PostgresRow&) = delete;
};

class PostgresResultSet
{
public:
    PostgresResultSet() = default;
    PostgresResultSet(PostgresResultSet&&) noexcept = default;
    PostgresResultSet& operator=(PostgresResultSet&&) noexcept = default;

    [[nodiscard]] PostgresResultSet clone() const;
    void addField(PostgresField field);
    void reserveFields(size_t count) { m_fields.reserve(count); }
    [[nodiscard]] size_t fieldCount() const noexcept { return m_fields.size(); }
    [[nodiscard]] const PostgresField& field(size_t index) const noexcept;
    [[nodiscard]] const std::vector<PostgresField>& fields() const noexcept { return m_fields; }
    void addRow(PostgresRow row);
    void reserveRows(size_t count) { m_rows.reserve(count); }
    [[nodiscard]] size_t rowCount() const noexcept { return m_rows.size(); }
    [[nodiscard]] const PostgresRow& row(size_t index) const noexcept;
    [[nodiscard]] const std::vector<PostgresRow>& rows() const noexcept { return m_rows; }
    [[nodiscard]] int findField(std::string_view name) const noexcept;

    void setCommandTag(std::string tag) { m_command_tag = std::move(tag); }
    void setAffectedRows(uint64_t count) noexcept { m_affected_rows = count; }
    [[nodiscard]] const std::string& commandTag() const noexcept { return m_command_tag; }
    [[nodiscard]] uint64_t affectedRows() const noexcept { return m_affected_rows; }
    [[nodiscard]] bool hasResultSet() const noexcept { return !m_fields.empty(); }

private:
    std::vector<PostgresField> m_fields;
    std::vector<PostgresRow> m_rows;
    std::string m_command_tag;
    uint64_t m_affected_rows = 0;

    PostgresResultSet(const PostgresResultSet&) = delete;
    PostgresResultSet& operator=(const PostgresResultSet&) = delete;
};

} // namespace galay::postgres

#endif // GALAY_POSTGRES_VALUE_H
