#include "postgres_value.h"

#include <charconv>
#include <utility>

namespace galay::postgres
{

PostgresField::PostgresField(std::string name,
                             uint32_t table_oid,
                             int16_t column_index,
                             uint32_t type_oid,
                             int16_t type_size,
                             int32_t type_modifier,
                             int16_t format)
    : m_name(std::move(name))
    , m_table_oid(table_oid)
    , m_type_oid(type_oid)
    , m_type_modifier(type_modifier)
    , m_column_index(column_index)
    , m_type_size(type_size)
    , m_format(format)
{
}

PostgresField PostgresField::clone() const
{
    return PostgresField(m_name,
                         m_table_oid,
                         m_column_index,
                         m_type_oid,
                         m_type_size,
                         m_type_modifier,
                         m_format);
}

PostgresRow::PostgresRow(std::vector<std::optional<std::string>> values)
    : m_values(std::move(values))
{
}

PostgresRow PostgresRow::clone() const
{
    return PostgresRow(m_values);
}

const std::optional<std::string>& PostgresRow::operator[](size_t index) const noexcept
{
    return m_values[index];
}

bool PostgresRow::isNull(size_t index) const noexcept
{
    return index >= m_values.size() || !m_values[index].has_value();
}

std::string PostgresRow::getString(size_t index, const std::string& default_value) const
{
    if (isNull(index)) {
        return default_value;
    }
    return *m_values[index];
}

int64_t PostgresRow::getInt64(size_t index, int64_t default_value) const noexcept
{
    if (isNull(index)) {
        return default_value;
    }
    const std::string& value = *m_values[index];
    int64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size()
        ? parsed
        : default_value;
}

uint64_t PostgresRow::getUint64(size_t index, uint64_t default_value) const noexcept
{
    if (isNull(index)) {
        return default_value;
    }
    const std::string& value = *m_values[index];
    uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size()
        ? parsed
        : default_value;
}

double PostgresRow::getDouble(size_t index, double default_value) const noexcept
{
    if (isNull(index)) {
        return default_value;
    }
    const std::string& value = *m_values[index];
    double parsed = 0.0;
    const auto result = std::from_chars(value.data(),
                                        value.data() + value.size(),
                                        parsed,
                                        std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size()
        ? parsed
        : default_value;
}

PostgresResultSet PostgresResultSet::clone() const
{
    PostgresResultSet copy;
    copy.m_fields.reserve(m_fields.size());
    for (const PostgresField& field : m_fields) {
        copy.m_fields.push_back(field.clone());
    }
    copy.m_rows.reserve(m_rows.size());
    for (const PostgresRow& row : m_rows) {
        copy.m_rows.push_back(row.clone());
    }
    copy.m_command_tag = m_command_tag;
    copy.m_affected_rows = m_affected_rows;
    return copy;
}

void PostgresResultSet::addField(PostgresField field)
{
    m_fields.push_back(std::move(field));
}

const PostgresField& PostgresResultSet::field(size_t index) const noexcept
{
    return m_fields[index];
}

void PostgresResultSet::addRow(PostgresRow row)
{
    m_rows.push_back(std::move(row));
}

const PostgresRow& PostgresResultSet::row(size_t index) const noexcept
{
    return m_rows[index];
}

int PostgresResultSet::findField(std::string_view name) const noexcept
{
    for (size_t index = 0; index < m_fields.size(); ++index) {
        if (m_fields[index].name() == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace galay::postgres
