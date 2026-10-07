#include "mongo_value.h"

#include <cmath>
#include <string_view>

namespace galay::mongo
{

namespace
{

bool is_hex_object_id(std::string_view oid) noexcept
{
    if (oid.size() != 24) {
        return false;
    }
    for (const char ch : oid) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

const std::string MongoValue::kEmptyString{};
const MongoValue::Binary MongoValue::kEmptyBinary{};

MongoValue::MongoValue()
    : m_storage(nullptr)
{
}

MongoValue::MongoValue(std::nullptr_t)
    : m_storage(nullptr)
{
}

MongoValue::MongoValue(bool value)
    : m_storage(value)
{
}

MongoValue::MongoValue(int32_t value)
    : m_storage(value)
{
}

MongoValue::MongoValue(int64_t value)
    : m_storage(value)
{
}

MongoValue::MongoValue(double value)
    : m_storage(value)
{
}

MongoValue::MongoValue(std::string value)
    : m_storage(std::move(value))
{
}

MongoValue::MongoValue(const char* value)
    : m_storage(std::string(value == nullptr ? "" : value))
{
}

MongoValue::MongoValue(Binary value)
    : m_storage(std::move(value))
{
}

MongoValue::MongoValue(MongoDocument value)
    : m_storage(std::make_shared<MongoDocument>(std::move(value)))
{
}

MongoValue::MongoValue(MongoArray value)
    : m_storage(std::make_shared<MongoArray>(std::move(value)))
{
}

MongoValue::MongoValue(ObjectIdTag, std::string oid)
    : m_storage(std::move(oid))
    , m_type_tag(MongoValueType::ObjectId)
{
}

MongoValue::MongoValue(DateTimeTag, int64_t millis)
    : m_storage(millis)
    , m_type_tag(MongoValueType::DateTime)
{
}

MongoValue::MongoValue(TimestampTag, uint64_t ts)
    : m_storage(static_cast<int64_t>(ts))
    , m_type_tag(MongoValueType::Timestamp)
{
}

std::expected<MongoValue, std::string> MongoValue::from_object_id(std::string oid)
{
    if (!is_hex_object_id(oid)) {
        return std::unexpected("ObjectId must be a 24-character hex string");
    }
    return MongoValue(ObjectIdTag{}, std::move(oid));
}

MongoValue MongoValue::from_date_time(int64_t millis)
{
    return MongoValue(DateTimeTag{}, millis);
}

MongoValue MongoValue::from_timestamp(uint64_t ts)
{
    return MongoValue(TimestampTag{}, ts);
}

MongoValue MongoValue::clone() const
{
    switch (type()) {
    case MongoValueType::Null:
        return MongoValue(nullptr);
    case MongoValueType::Bool:
        return MongoValue(std::get<bool>(m_storage));
    case MongoValueType::Int32:
        return MongoValue(std::get<int32_t>(m_storage));
    case MongoValueType::Int64:
        return MongoValue(std::get<int64_t>(m_storage));
    case MongoValueType::Double:
        return MongoValue(std::get<double>(m_storage));
    case MongoValueType::String:
        return MongoValue(std::get<std::string>(m_storage));
    case MongoValueType::Binary:
        return MongoValue(std::get<Binary>(m_storage));
    case MongoValueType::Document:
        return MongoValue(to_document().clone());
    case MongoValueType::Array:
        return MongoValue(to_array().clone());
    case MongoValueType::ObjectId:
        return MongoValue(ObjectIdTag{}, std::get<std::string>(m_storage));
    case MongoValueType::DateTime:
        return MongoValue(DateTimeTag{}, std::get<int64_t>(m_storage));
    case MongoValueType::Timestamp:
        return MongoValue(TimestampTag{}, static_cast<uint64_t>(std::get<int64_t>(m_storage)));
    }
    return MongoValue(nullptr);
}

MongoValueType MongoValue::type() const
{
    if (m_type_tag == MongoValueType::ObjectId) return MongoValueType::ObjectId;
    if (m_type_tag == MongoValueType::DateTime) return MongoValueType::DateTime;
    if (m_type_tag == MongoValueType::Timestamp) return MongoValueType::Timestamp;
    if (std::holds_alternative<std::nullptr_t>(m_storage)) return MongoValueType::Null;
    if (std::holds_alternative<bool>(m_storage)) return MongoValueType::Bool;
    if (std::holds_alternative<int32_t>(m_storage)) return MongoValueType::Int32;
    if (std::holds_alternative<int64_t>(m_storage)) return MongoValueType::Int64;
    if (std::holds_alternative<double>(m_storage)) return MongoValueType::Double;
    if (std::holds_alternative<std::string>(m_storage)) return MongoValueType::String;
    if (std::holds_alternative<Binary>(m_storage)) return MongoValueType::Binary;
    if (std::holds_alternative<DocumentPtr>(m_storage)) return MongoValueType::Document;
    return MongoValueType::Array;
}

bool MongoValue::is_null() const { return std::holds_alternative<std::nullptr_t>(m_storage); }
bool MongoValue::is_bool() const { return std::holds_alternative<bool>(m_storage); }
bool MongoValue::is_int32() const { return std::holds_alternative<int32_t>(m_storage); }
bool MongoValue::is_int64() const { return std::holds_alternative<int64_t>(m_storage); }
bool MongoValue::is_double() const { return std::holds_alternative<double>(m_storage); }
bool MongoValue::is_string() const { return std::holds_alternative<std::string>(m_storage); }
bool MongoValue::is_binary() const { return std::holds_alternative<Binary>(m_storage); }
bool MongoValue::is_document() const { return std::holds_alternative<DocumentPtr>(m_storage); }
bool MongoValue::is_array() const { return std::holds_alternative<ArrayPtr>(m_storage); }
bool MongoValue::is_object_id() const { return m_type_tag == MongoValueType::ObjectId; }
bool MongoValue::is_date_time() const { return m_type_tag == MongoValueType::DateTime; }
bool MongoValue::is_timestamp() const { return m_type_tag == MongoValueType::Timestamp; }

bool MongoValue::to_bool(bool default_value) const
{
    if (is_bool()) return std::get<bool>(m_storage);
    if (is_int32()) return std::get<int32_t>(m_storage) != 0;
    if (is_int64()) return std::get<int64_t>(m_storage) != 0;
    if (is_double()) return std::fabs(std::get<double>(m_storage)) > 1e-12;
    return default_value;
}

int32_t MongoValue::to_int32(int32_t default_value) const
{
    if (is_int32()) return std::get<int32_t>(m_storage);
    if (is_int64()) return static_cast<int32_t>(std::get<int64_t>(m_storage));
    if (is_double()) return static_cast<int32_t>(std::get<double>(m_storage));
    if (is_bool()) return std::get<bool>(m_storage) ? 1 : 0;
    return default_value;
}

int64_t MongoValue::to_int64(int64_t default_value) const
{
    if (is_int64()) return std::get<int64_t>(m_storage);
    if (is_int32()) return std::get<int32_t>(m_storage);
    if (is_double()) return static_cast<int64_t>(std::get<double>(m_storage));
    if (is_bool()) return std::get<bool>(m_storage) ? 1 : 0;
    return default_value;
}

double MongoValue::to_double(double default_value) const
{
    if (is_double()) return std::get<double>(m_storage);
    if (is_int64()) return static_cast<double>(std::get<int64_t>(m_storage));
    if (is_int32()) return static_cast<double>(std::get<int32_t>(m_storage));
    if (is_bool()) return std::get<bool>(m_storage) ? 1.0 : 0.0;
    return default_value;
}

const std::string& MongoValue::to_string() const
{
    if (is_string()) return std::get<std::string>(m_storage);
    return kEmptyString;
}

const MongoValue::Binary& MongoValue::to_binary() const
{
    if (is_binary()) return std::get<Binary>(m_storage);
    return kEmptyBinary;
}

const MongoDocument& MongoValue::to_document() const
{
    if (is_document()) {
        const auto& ptr = std::get<DocumentPtr>(m_storage);
        if (ptr) return *ptr;
    }
    static const MongoDocument kEmptyDocument;
    return kEmptyDocument;
}

const MongoArray& MongoValue::to_array() const
{
    if (is_array()) {
        const auto& ptr = std::get<ArrayPtr>(m_storage);
        if (ptr) return *ptr;
    }
    static const MongoArray kEmptyArray;
    return kEmptyArray;
}

MongoDocument& MongoValue::as_document()
{
    if (!is_document() || !std::get<DocumentPtr>(m_storage)) {
        m_storage = std::make_shared<MongoDocument>();
    }
    return *std::get<DocumentPtr>(m_storage);
}

MongoArray& MongoValue::as_array()
{
    if (!is_array() || !std::get<ArrayPtr>(m_storage)) {
        m_storage = std::make_shared<MongoArray>();
    }
    return *std::get<ArrayPtr>(m_storage);
}

MongoArray::MongoArray(std::vector<MongoValue> values)
    : m_values(std::move(values))
{
}

MongoArray MongoArray::clone() const
{
    MongoArray copy;
    copy.reserve(m_values.size());
    for (const auto& value : m_values) {
        copy.append(value.clone());
    }
    return copy;
}

void MongoArray::append(MongoValue value)
{
    m_values.push_back(std::move(value));
}

void MongoArray::reserve(size_t n)
{
    m_values.reserve(n);
}

size_t MongoArray::size() const
{
    return m_values.size();
}

bool MongoArray::empty() const
{
    return m_values.empty();
}

std::expected<std::reference_wrapper<const MongoValue>, std::string> MongoArray::at(size_t index) const
{
    if (index >= m_values.size()) {
        return std::unexpected("MongoArray index out of range");
    }
    return std::cref(m_values[index]);
}

const MongoValue& MongoArray::operator[](size_t index) const
{
    return m_values[index];
}

const std::vector<MongoValue>& MongoArray::values() const
{
    return m_values;
}

std::vector<MongoValue>& MongoArray::values()
{
    return m_values;
}

MongoDocument::MongoDocument(std::vector<Field> fields)
    : m_fields(std::move(fields))
{
}

MongoDocument MongoDocument::clone() const
{
    MongoDocument copy;
    copy.fields().reserve(m_fields.size());
    for (const auto& [name, value] : m_fields) {
        copy.append(name, value.clone());
    }
    return copy;
}

void MongoDocument::append(std::string key, MongoValue value)
{
    m_fields.emplace_back(std::move(key), std::move(value));
}

void MongoDocument::set(std::string key, MongoValue value)
{
    for (auto& [name, existing] : m_fields) {
        if (name == key) {
            existing = std::move(value);
            return;
        }
    }
    append(std::move(key), std::move(value));
}

bool MongoDocument::has(const std::string& key) const
{
    return find(key) != nullptr;
}

const MongoValue* MongoDocument::find(const std::string& key) const
{
    for (const auto& [name, value] : m_fields) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

MongoValue* MongoDocument::find(const std::string& key)
{
    for (auto& [name, value] : m_fields) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::expected<std::reference_wrapper<const MongoValue>, std::string> MongoDocument::at(const std::string& key) const
{
    const auto* value = find(key);
    if (!value) {
        return std::unexpected("MongoDocument key not found: " + key);
    }
    return std::cref(*value);
}

std::string MongoDocument::get_string(const std::string& key, std::string default_value) const
{
    const auto* value = find(key);
    if (!value || !value->is_string()) {
        return default_value;
    }
    return value->to_string();
}

int32_t MongoDocument::get_int32(const std::string& key, int32_t default_value) const
{
    const auto* value = find(key);
    return value ? value->to_int32(default_value) : default_value;
}

int64_t MongoDocument::get_int64(const std::string& key, int64_t default_value) const
{
    const auto* value = find(key);
    return value ? value->to_int64(default_value) : default_value;
}

double MongoDocument::get_double(const std::string& key, double default_value) const
{
    const auto* value = find(key);
    return value ? value->to_double(default_value) : default_value;
}

bool MongoDocument::get_bool(const std::string& key, bool default_value) const
{
    const auto* value = find(key);
    return value ? value->to_bool(default_value) : default_value;
}

size_t MongoDocument::size() const
{
    return m_fields.size();
}

bool MongoDocument::empty() const
{
    return m_fields.empty();
}

const std::vector<MongoDocument::Field>& MongoDocument::fields() const
{
    return m_fields;
}

std::vector<MongoDocument::Field>& MongoDocument::fields()
{
    return m_fields;
}

MongoReply::MongoReply(MongoDocument document)
    : m_document(std::move(document))
{
}

MongoReply MongoReply::clone() const
{
    return MongoReply(m_document.clone());
}

const MongoDocument& MongoReply::document() const
{
    return m_document;
}

MongoDocument& MongoReply::document()
{
    return m_document;
}

bool MongoReply::ok() const
{
    const auto* ok_field = m_document.find("ok");
    if (!ok_field) {
        return false;
    }
    if (ok_field->is_bool()) {
        return ok_field->to_bool(false);
    }
    return ok_field->to_double(0.0) >= 1.0;
}

bool MongoReply::has_command_error() const
{
    return !ok();
}

int32_t MongoReply::error_code() const
{
    return m_document.get_int32("code", 0);
}

std::string MongoReply::error_message() const
{
    const auto message = m_document.get_string("errmsg", "");
    if (!message.empty()) {
        return message;
    }
    return m_document.get_string("$err", "");
}

} // namespace galay::mongo
