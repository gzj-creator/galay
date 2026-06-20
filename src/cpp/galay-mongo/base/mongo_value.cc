#include "mongo_value.h"

#include <cmath>
#include <stdexcept>

namespace galay::mongo
{

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

MongoValue MongoValue::fromObjectId(std::string oid)
{
    return MongoValue(ObjectIdTag{}, std::move(oid));
}

MongoValue MongoValue::fromDateTime(int64_t millis)
{
    return MongoValue(DateTimeTag{}, millis);
}

MongoValue MongoValue::fromTimestamp(uint64_t ts)
{
    return MongoValue(TimestampTag{}, ts);
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

bool MongoValue::isNull() const { return std::holds_alternative<std::nullptr_t>(m_storage); }
bool MongoValue::isBool() const { return std::holds_alternative<bool>(m_storage); }
bool MongoValue::isInt32() const { return std::holds_alternative<int32_t>(m_storage); }
bool MongoValue::isInt64() const { return std::holds_alternative<int64_t>(m_storage); }
bool MongoValue::isDouble() const { return std::holds_alternative<double>(m_storage); }
bool MongoValue::isString() const { return std::holds_alternative<std::string>(m_storage); }
bool MongoValue::isBinary() const { return std::holds_alternative<Binary>(m_storage); }
bool MongoValue::isDocument() const { return std::holds_alternative<DocumentPtr>(m_storage); }
bool MongoValue::isArray() const { return std::holds_alternative<ArrayPtr>(m_storage); }
bool MongoValue::isObjectId() const { return m_type_tag == MongoValueType::ObjectId; }
bool MongoValue::isDateTime() const { return m_type_tag == MongoValueType::DateTime; }
bool MongoValue::isTimestamp() const { return m_type_tag == MongoValueType::Timestamp; }

bool MongoValue::toBool(bool default_value) const
{
    if (isBool()) return std::get<bool>(m_storage);
    if (isInt32()) return std::get<int32_t>(m_storage) != 0;
    if (isInt64()) return std::get<int64_t>(m_storage) != 0;
    if (isDouble()) return std::fabs(std::get<double>(m_storage)) > 1e-12;
    return default_value;
}

int32_t MongoValue::toInt32(int32_t default_value) const
{
    if (isInt32()) return std::get<int32_t>(m_storage);
    if (isInt64()) return static_cast<int32_t>(std::get<int64_t>(m_storage));
    if (isDouble()) return static_cast<int32_t>(std::get<double>(m_storage));
    if (isBool()) return std::get<bool>(m_storage) ? 1 : 0;
    return default_value;
}

int64_t MongoValue::toInt64(int64_t default_value) const
{
    if (isInt64()) return std::get<int64_t>(m_storage);
    if (isInt32()) return std::get<int32_t>(m_storage);
    if (isDouble()) return static_cast<int64_t>(std::get<double>(m_storage));
    if (isBool()) return std::get<bool>(m_storage) ? 1 : 0;
    return default_value;
}

double MongoValue::toDouble(double default_value) const
{
    if (isDouble()) return std::get<double>(m_storage);
    if (isInt64()) return static_cast<double>(std::get<int64_t>(m_storage));
    if (isInt32()) return static_cast<double>(std::get<int32_t>(m_storage));
    if (isBool()) return std::get<bool>(m_storage) ? 1.0 : 0.0;
    return default_value;
}

const std::string& MongoValue::toString() const
{
    if (isString()) return std::get<std::string>(m_storage);
    return kEmptyString;
}

const MongoValue::Binary& MongoValue::toBinary() const
{
    if (isBinary()) return std::get<Binary>(m_storage);
    return kEmptyBinary;
}

const MongoDocument& MongoValue::toDocument() const
{
    if (isDocument()) {
        const auto& ptr = std::get<DocumentPtr>(m_storage);
        if (ptr) return *ptr;
    }
    static const MongoDocument kEmptyDocument;
    return kEmptyDocument;
}

const MongoArray& MongoValue::toArray() const
{
    if (isArray()) {
        const auto& ptr = std::get<ArrayPtr>(m_storage);
        if (ptr) return *ptr;
    }
    static const MongoArray kEmptyArray;
    return kEmptyArray;
}

MongoDocument& MongoValue::asDocument()
{
    if (!isDocument() || !std::get<DocumentPtr>(m_storage)) {
        m_storage = std::make_shared<MongoDocument>();
    }
    return *std::get<DocumentPtr>(m_storage);
}

MongoArray& MongoValue::asArray()
{
    if (!isArray() || !std::get<ArrayPtr>(m_storage)) {
        m_storage = std::make_shared<MongoArray>();
    }
    return *std::get<ArrayPtr>(m_storage);
}

MongoArray::MongoArray(std::vector<MongoValue> values)
    : m_values(std::move(values))
{
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

const MongoValue& MongoArray::at(size_t index) const
{
    return m_values.at(index);
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

const MongoValue& MongoDocument::at(const std::string& key) const
{
    const auto* value = find(key);
    if (!value) {
        throw std::out_of_range("MongoDocument key not found: " + key);
    }
    return *value;
}

std::string MongoDocument::getString(const std::string& key, std::string default_value) const
{
    const auto* value = find(key);
    if (!value || !value->isString()) {
        return default_value;
    }
    return value->toString();
}

int32_t MongoDocument::getInt32(const std::string& key, int32_t default_value) const
{
    const auto* value = find(key);
    return value ? value->toInt32(default_value) : default_value;
}

int64_t MongoDocument::getInt64(const std::string& key, int64_t default_value) const
{
    const auto* value = find(key);
    return value ? value->toInt64(default_value) : default_value;
}

double MongoDocument::getDouble(const std::string& key, double default_value) const
{
    const auto* value = find(key);
    return value ? value->toDouble(default_value) : default_value;
}

bool MongoDocument::getBool(const std::string& key, bool default_value) const
{
    const auto* value = find(key);
    return value ? value->toBool(default_value) : default_value;
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
    if (ok_field->isBool()) {
        return ok_field->toBool(false);
    }
    return ok_field->toDouble(0.0) >= 1.0;
}

bool MongoReply::hasCommandError() const
{
    return !ok();
}

int32_t MongoReply::errorCode() const
{
    return m_document.getInt32("code", 0);
}

std::string MongoReply::errorMessage() const
{
    const auto message = m_document.getString("errmsg", "");
    if (!message.empty()) {
        return message;
    }
    return m_document.getString("$err", "");
}

} // namespace galay::mongo
