#include "mysql_value.h"
#include <stdexcept>

namespace galay::mysql
{

// ======================== MysqlField ========================

MysqlField::MysqlField(std::string name, MysqlFieldType type, uint16_t flags,
                       uint32_t column_length, uint8_t decimals)
    : m_name(std::move(name))
    , m_column_length(column_length)
    , m_flags(flags)
    , m_type(type)
    , m_decimals(decimals)
{
}

// ======================== MysqlRow ========================

MysqlRow::MysqlRow(std::vector<std::optional<std::string>> values)
    : m_values(std::move(values))
{
}

const std::optional<std::string>& MysqlRow::operator[](size_t index) const
{
    return m_values[index];
}

const std::optional<std::string>& MysqlRow::at(size_t index) const
{
    if (index >= m_values.size()) {
        throw std::out_of_range("MysqlRow index out of range");
    }
    return m_values[index];
}

bool MysqlRow::isNull(size_t index) const
{
    if (index >= m_values.size()) return true;
    return !m_values[index].has_value();
}

std::string MysqlRow::getString(size_t index, const std::string& default_val) const
{
    if (index >= m_values.size() || !m_values[index].has_value()) {
        return default_val;
    }
    return m_values[index].value();
}

int64_t MysqlRow::getInt64(size_t index, int64_t default_val) const
{
    if (index >= m_values.size() || !m_values[index].has_value()) {
        return default_val;
    }
    try {
        return std::stoll(m_values[index].value());
    } catch (...) {
        return default_val;
    }
}

uint64_t MysqlRow::getUint64(size_t index, uint64_t default_val) const
{
    if (index >= m_values.size() || !m_values[index].has_value()) {
        return default_val;
    }
    try {
        return std::stoull(m_values[index].value());
    } catch (...) {
        return default_val;
    }
}

double MysqlRow::getDouble(size_t index, double default_val) const
{
    if (index >= m_values.size() || !m_values[index].has_value()) {
        return default_val;
    }
    try {
        return std::stod(m_values[index].value());
    } catch (...) {
        return default_val;
    }
}

// ======================== MysqlResultSet ========================

void MysqlResultSet::addField(MysqlField field)
{
    m_fields.push_back(std::move(field));
}

const MysqlField& MysqlResultSet::field(size_t index) const
{
    return m_fields.at(index);
}

void MysqlResultSet::addRow(MysqlRow row)
{
    m_rows.push_back(std::move(row));
}

const MysqlRow& MysqlResultSet::row(size_t index) const
{
    return m_rows.at(index);
}

int MysqlResultSet::findField(const std::string& name) const
{
    for (size_t i = 0; i < m_fields.size(); ++i) {
        if (m_fields[i].name() == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace galay::mysql
