#include "bson.h"

#include <charconv>
#include <array>
#include <cstring>
#include <stdexcept>

namespace galay::mongo::protocol
{

namespace
{
constexpr uint8_t kBinarySubtypeGeneric = 0x00;

int32_t readInt32LEUnchecked(const char* p)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24));
}

int64_t readInt64LEUnchecked(const char* p)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= (static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i));
    }
    return static_cast<int64_t>(value);
}

void writeInt32LE(std::string& out, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out.push_back(static_cast<char>(u & 0xFF));
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
    out.push_back(static_cast<char>((u >> 16) & 0xFF));
    out.push_back(static_cast<char>((u >> 24) & 0xFF));
}

void writeInt32LEAt(std::string& out, size_t pos, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out[pos + 0] = static_cast<char>(u & 0xFF);
    out[pos + 1] = static_cast<char>((u >> 8) & 0xFF);
    out[pos + 2] = static_cast<char>((u >> 16) & 0xFF);
    out[pos + 3] = static_cast<char>((u >> 24) & 0xFF);
}

void writeInt64LE(std::string& out, int64_t value)
{
    const auto u = static_cast<uint64_t>(value);
    for (size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
    }
}

void writeDoubleLE(std::string& out, double value)
{
    static_assert(sizeof(double) == sizeof(uint64_t), "Unexpected double size");
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeInt64LE(out, static_cast<int64_t>(bits));
}

MongoArray decodeArrayFromDocument(const MongoDocument& array_as_document)
{
    MongoArray array;
    array.reserve(array_as_document.size());
    for (const auto& [_, value] : array_as_document.fields()) {
        array.append(value);
    }
    return array;
}

} // namespace

std::string BsonCodec::encodeDocument(const MongoDocument& document)
{
    std::string out;
    out.reserve(64);
    appendDocument(out, document);
    return out;
}

void BsonCodec::appendDocument(std::string& out, const MongoDocument& document)
{
    const size_t base = out.size();
    out.resize(base + 4, '\0');

    for (const auto& [key, value] : document.fields()) {
        encodeElement(out, key, value);
    }

    out.push_back('\0');

    const auto total_len = static_cast<int32_t>(out.size() - base);
    writeInt32LEAt(out, base, total_len);
}

void BsonCodec::appendDocumentWithDatabase(std::string& out,
                                           const MongoDocument& document,
                                           std::string_view database)
{
    const size_t base = out.size();
    out.resize(base + 4, '\0');

    bool has_db = false;
    for (const auto& [key, value] : document.fields()) {
        if (!has_db && key == "$db") {
            has_db = true;
        }
        encodeElement(out, key, value);
    }

    if (!has_db) {
        out.push_back(static_cast<char>(BsonType::String));
        writeCString(out, "$db");
        writeInt32(out, static_cast<int32_t>(database.size() + 1));
        out.append(database.data(), database.size());
        out.push_back('\0');
    }

    out.push_back('\0');

    const auto total_len = static_cast<int32_t>(out.size() - base);
    writeInt32LEAt(out, base, total_len);
}

std::expected<MongoDocument, std::string> BsonCodec::decodeDocument(const char* data, size_t len)
{
    size_t consumed = 0;
    return decodeDocument(data, len, consumed);
}

std::expected<MongoDocument, std::string>
BsonCodec::decodeDocument(const char* data, size_t len, size_t& consumed)
{
    if (data == nullptr || len < 5) {
        return std::unexpected("BSON document too short");
    }

    auto total_len_or_err = readInt32(data, len, 0);
    if (!total_len_or_err) {
        return std::unexpected(total_len_or_err.error());
    }

    const int32_t total_len = total_len_or_err.value();
    if (total_len < 5) {
        return std::unexpected("Invalid BSON length");
    }
    if (static_cast<size_t>(total_len) > len) {
        return std::unexpected("Incomplete BSON document");
    }
    if (data[total_len - 1] != '\0') {
        return std::unexpected("BSON document is not null terminated");
    }

    MongoDocument document;
    size_t pos = 4;

    while (pos < static_cast<size_t>(total_len - 1)) {
        const auto type = static_cast<BsonType>(static_cast<uint8_t>(data[pos++]));

        auto key_or_err = readCString(data, total_len, pos);
        if (!key_or_err) {
            return std::unexpected(key_or_err.error());
        }

        auto value_or_err = decodeElementValue(type, data, total_len, pos);
        if (!value_or_err) {
            return std::unexpected(value_or_err.error());
        }

        document.append(std::move(key_or_err.value()), std::move(value_or_err.value()));
    }

    consumed = static_cast<size_t>(total_len);
    return document;
}

void BsonCodec::writeInt32(std::string& out, int32_t value)
{
    writeInt32LE(out, value);
}

void BsonCodec::writeInt64(std::string& out, int64_t value)
{
    writeInt64LE(out, value);
}

void BsonCodec::writeDouble(std::string& out, double value)
{
    writeDoubleLE(out, value);
}

void BsonCodec::writeCString(std::string& out, std::string_view value)
{
    if (value.find('\0') != std::string::npos) {
        throw std::invalid_argument("BSON CString must not contain embedded NUL bytes");
    }
    out.append(value.data(), value.size());
    out.push_back('\0');
}

std::expected<int32_t, std::string> BsonCodec::readInt32(const char* data, size_t len, size_t pos)
{
    if (pos + 4 > len) {
        return std::unexpected("readInt32 out of range");
    }
    return readInt32LEUnchecked(data + pos);
}

std::expected<int64_t, std::string> BsonCodec::readInt64(const char* data, size_t len, size_t pos)
{
    if (pos + 8 > len) {
        return std::unexpected("readInt64 out of range");
    }
    return readInt64LEUnchecked(data + pos);
}

std::expected<double, std::string> BsonCodec::readDouble(const char* data, size_t len, size_t pos)
{
    auto bits_or_err = readInt64(data, len, pos);
    if (!bits_or_err) {
        return std::unexpected(bits_or_err.error());
    }

    const uint64_t bits = static_cast<uint64_t>(bits_or_err.value());
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::expected<std::string, std::string> BsonCodec::readCString(const char* data, size_t len, size_t& pos)
{
    if (pos >= len) {
        return std::unexpected("readCString out of range");
    }

    size_t start = pos;
    while (pos < len && data[pos] != '\0') {
        ++pos;
    }

    if (pos >= len) {
        return std::unexpected("CString terminator not found");
    }

    std::string value(data + start, pos - start);
    ++pos; // skip '\0'
    return value;
}

void BsonCodec::encodeElement(std::string& out, std::string_view key, const MongoValue& value)
{
    switch (value.type()) {
    case MongoValueType::Double:
        out.push_back(static_cast<char>(BsonType::Double));
        writeCString(out, key);
        writeDouble(out, value.toDouble());
        break;
    case MongoValueType::String: {
        out.push_back(static_cast<char>(BsonType::String));
        writeCString(out, key);
        const auto& text = value.toString();
        writeInt32(out, static_cast<int32_t>(text.size() + 1));
        out.append(text);
        out.push_back('\0');
        break;
    }
    case MongoValueType::Document: {
        out.push_back(static_cast<char>(BsonType::Document));
        writeCString(out, key);
        appendDocument(out, value.toDocument());
        break;
    }
    case MongoValueType::Array: {
        out.push_back(static_cast<char>(BsonType::Array));
        writeCString(out, key);
        const size_t base = out.size();
        out.resize(base + 4, '\0');
        const auto& values = value.toArray().values();
        for (size_t i = 0; i < values.size(); ++i) {
            std::array<char, 24> key_buf{};
            const auto result =
                std::to_chars(key_buf.data(), key_buf.data() + key_buf.size(), i);
            if (result.ec != std::errc()) {
                throw std::runtime_error("failed to encode BSON array index");
            }
            const std::string_view index_key(
                key_buf.data(),
                static_cast<size_t>(result.ptr - key_buf.data()));
            encodeElement(out, index_key, values[i]);
        }
        out.push_back('\0');
        const auto total_len = static_cast<int32_t>(out.size() - base);
        writeInt32LEAt(out, base, total_len);
        break;
    }
    case MongoValueType::Binary: {
        out.push_back(static_cast<char>(BsonType::Binary));
        writeCString(out, key);
        const auto& binary = value.toBinary();
        writeInt32(out, static_cast<int32_t>(binary.size()));
        out.push_back(static_cast<char>(kBinarySubtypeGeneric));
        out.append(reinterpret_cast<const char*>(binary.data()), binary.size());
        break;
    }
    case MongoValueType::Bool:
        out.push_back(static_cast<char>(BsonType::Bool));
        writeCString(out, key);
        out.push_back(value.toBool(false) ? 1 : 0);
        break;
    case MongoValueType::Null:
        out.push_back(static_cast<char>(BsonType::Null));
        writeCString(out, key);
        break;
    case MongoValueType::Int32:
        out.push_back(static_cast<char>(BsonType::Int32));
        writeCString(out, key);
        writeInt32(out, value.toInt32());
        break;
    case MongoValueType::Int64:
        out.push_back(static_cast<char>(BsonType::Int64));
        writeCString(out, key);
        writeInt64(out, value.toInt64());
        break;
    case MongoValueType::ObjectId: {
        out.push_back(static_cast<char>(BsonType::ObjectId));
        writeCString(out, key);
        // Decode 24-char hex string back to 12 raw bytes
        const auto& oid_hex = value.toString();
        for (size_t i = 0; i + 1 < oid_hex.size(); i += 2) {
            auto hi = static_cast<uint8_t>(oid_hex[i]);
            auto lo = static_cast<uint8_t>(oid_hex[i + 1]);
            auto hex_to_nibble = [](uint8_t c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return 0;
            };
            out.push_back(static_cast<char>((hex_to_nibble(hi) << 4) | hex_to_nibble(lo)));
        }
        break;
    }
    case MongoValueType::DateTime:
        out.push_back(static_cast<char>(BsonType::DateTime));
        writeCString(out, key);
        writeInt64(out, value.toInt64());
        break;
    case MongoValueType::Timestamp:
        out.push_back(static_cast<char>(BsonType::Timestamp));
        writeCString(out, key);
        writeInt64(out, value.toInt64());
        break;
    }
}

std::expected<MongoValue, std::string> BsonCodec::decodeElementValue(BsonType type,
                                                                      const char* data,
                                                                      size_t len,
                                                                      size_t& pos)
{
    switch (type) {
    case BsonType::Double: {
        auto d = readDouble(data, len, pos);
        if (!d) return std::unexpected(d.error());
        pos += 8;
        return MongoValue(d.value());
    }
    case BsonType::String: {
        auto str_len_or_err = readInt32(data, len, pos);
        if (!str_len_or_err) {
            return std::unexpected(str_len_or_err.error());
        }
        const int32_t str_len = str_len_or_err.value();
        if (str_len <= 0) {
            return std::unexpected("Invalid BSON string length");
        }
        pos += 4;
        if (pos + static_cast<size_t>(str_len) > len) {
            return std::unexpected("BSON string out of range");
        }
        if (data[pos + str_len - 1] != '\0') {
            return std::unexpected("BSON string not null terminated");
        }
        std::string text(data + pos, str_len - 1);
        pos += static_cast<size_t>(str_len);
        return MongoValue(std::move(text));
    }
    case BsonType::Document: {
        size_t consumed = 0;
        auto doc_or_err = decodeDocument(data + pos, len - pos, consumed);
        if (!doc_or_err) {
            return std::unexpected(doc_or_err.error());
        }
        pos += consumed;
        return MongoValue(std::move(doc_or_err.value()));
    }
    case BsonType::Array: {
        size_t consumed = 0;
        auto doc_or_err = decodeDocument(data + pos, len - pos, consumed);
        if (!doc_or_err) {
            return std::unexpected(doc_or_err.error());
        }
        pos += consumed;
        return MongoValue(decodeArrayFromDocument(doc_or_err.value()));
    }
    case BsonType::Binary: {
        auto blob_len_or_err = readInt32(data, len, pos);
        if (!blob_len_or_err) {
            return std::unexpected(blob_len_or_err.error());
        }
        const int32_t blob_len = blob_len_or_err.value();
        if (blob_len < 0) {
            return std::unexpected("Invalid BSON binary length");
        }
        pos += 4;
        if (pos + 1 + static_cast<size_t>(blob_len) > len) {
            return std::unexpected("BSON binary out of range");
        }
        ++pos; // subtype
        MongoValue::Binary binary(static_cast<size_t>(blob_len));
        if (blob_len > 0) {
            std::memcpy(binary.data(), data + pos, static_cast<size_t>(blob_len));
        }
        pos += static_cast<size_t>(blob_len);
        return MongoValue(std::move(binary));
    }
    case BsonType::ObjectId: {
        if (pos + 12 > len) {
            return std::unexpected("BSON ObjectId out of range");
        }
        // Encode 12 raw bytes as 24-char hex string
        static constexpr char hex_chars[] = "0123456789abcdef";
        std::string oid;
        oid.reserve(24);
        for (size_t i = 0; i < 12; ++i) {
            const auto byte = static_cast<uint8_t>(data[pos + i]);
            oid.push_back(hex_chars[byte >> 4]);
            oid.push_back(hex_chars[byte & 0x0F]);
        }
        pos += 12;
        return MongoValue::fromObjectId(std::move(oid));
    }
    case BsonType::Bool: {
        if (pos + 1 > len) {
            return std::unexpected("BSON bool out of range");
        }
        const bool value = data[pos++] != 0;
        return MongoValue(value);
    }
    case BsonType::DateTime: {
        auto ts = readInt64(data, len, pos);
        if (!ts) return std::unexpected(ts.error());
        pos += 8;
        return MongoValue::fromDateTime(ts.value());
    }
    case BsonType::Null:
        return MongoValue(nullptr);
    case BsonType::Int32: {
        auto i32 = readInt32(data, len, pos);
        if (!i32) return std::unexpected(i32.error());
        pos += 4;
        return MongoValue(i32.value());
    }
    case BsonType::Timestamp: {
        auto ts = readInt64(data, len, pos);
        if (!ts) return std::unexpected(ts.error());
        pos += 8;
        return MongoValue::fromTimestamp(static_cast<uint64_t>(ts.value()));
    }
    case BsonType::Int64: {
        auto i64 = readInt64(data, len, pos);
        if (!i64) return std::unexpected(i64.error());
        pos += 8;
        return MongoValue(i64.value());
    }
    default:
        return std::unexpected("Unsupported BSON type: " + std::to_string(static_cast<int>(type)));
    }
}

} // namespace galay::mongo::protocol
