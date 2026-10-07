#include "bson.h"

#include <charconv>
#include <array>
#include <cstring>

namespace galay::mongo::protocol
{

namespace
{
constexpr uint8_t kBinarySubtypeGeneric = 0x00;

int32_t read_int32_le_unchecked(const char* p)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24));
}

int64_t read_int64_le_unchecked(const char* p)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= (static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i));
    }
    return static_cast<int64_t>(value);
}

void write_int32_le(std::string& out, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out.push_back(static_cast<char>(u & 0xFF));
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
    out.push_back(static_cast<char>((u >> 16) & 0xFF));
    out.push_back(static_cast<char>((u >> 24) & 0xFF));
}

void write_int32_le_at(std::string& out, size_t pos, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out[pos + 0] = static_cast<char>(u & 0xFF);
    out[pos + 1] = static_cast<char>((u >> 8) & 0xFF);
    out[pos + 2] = static_cast<char>((u >> 16) & 0xFF);
    out[pos + 3] = static_cast<char>((u >> 24) & 0xFF);
}

void write_int64_le(std::string& out, int64_t value)
{
    const auto u = static_cast<uint64_t>(value);
    for (size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
    }
}

void write_double_le(std::string& out, double value)
{
    static_assert(sizeof(double) == sizeof(uint64_t), "Unexpected double size");
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_int64_le(out, static_cast<int64_t>(bits));
}

MongoArray decode_array_from_document(MongoDocument array_as_document)
{
    MongoArray array;
    array.reserve(array_as_document.size());
    for (auto& [_, value] : array_as_document.fields()) {
        array.append(std::move(value));
    }
    return array;
}

} // namespace

std::expected<std::string, std::string> BsonCodec::encode_document(const MongoDocument& document)
{
    std::string out;
    out.reserve(64);
    auto appended = append_document(out, document);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    return out;
}

std::expected<void, std::string> BsonCodec::append_document(std::string& out,
                                                           const MongoDocument& document)
{
    const size_t base = out.size();
    out.resize(base + 4, '\0');

    for (const auto& [key, value] : document.fields()) {
        auto encoded = encode_element(out, key, value);
        if (!encoded) {
            out.resize(base);
            return std::unexpected(encoded.error());
        }
    }

    out.push_back('\0');

    const auto total_len = static_cast<int32_t>(out.size() - base);
    write_int32_le_at(out, base, total_len);
    return {};
}

std::expected<void, std::string> BsonCodec::append_document_with_database(std::string& out,
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
        auto encoded = encode_element(out, key, value);
        if (!encoded) {
            out.resize(base);
            return std::unexpected(encoded.error());
        }
    }

    if (!has_db) {
        out.push_back(static_cast<char>(BsonType::String));
        auto key_written = write_c_string(out, "$db");
        if (!key_written) {
            out.resize(base);
            return std::unexpected(key_written.error());
        }
        write_int32(out, static_cast<int32_t>(database.size() + 1));
        out.append(database.data(), database.size());
        out.push_back('\0');
    }

    out.push_back('\0');

    const auto total_len = static_cast<int32_t>(out.size() - base);
    write_int32_le_at(out, base, total_len);
    return {};
}

std::expected<MongoDocument, std::string> BsonCodec::decode_document(const char* data, size_t len)
{
    size_t consumed = 0;
    return decode_document(data, len, consumed);
}

std::expected<MongoDocument, std::string>
BsonCodec::decode_document(const char* data, size_t len, size_t& consumed)
{
    if (data == nullptr || len < 5) {
        return std::unexpected("BSON document too short");
    }

    auto total_len_or_err = read_int32(data, len, 0);
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

        auto key_or_err = read_c_string(data, total_len, pos);
        if (!key_or_err) {
            return std::unexpected(key_or_err.error());
        }

        auto value_or_err = decode_element_value(type, data, total_len, pos);
        if (!value_or_err) {
            return std::unexpected(value_or_err.error());
        }

        document.append(std::move(key_or_err.value()), std::move(value_or_err.value()));
    }

    consumed = static_cast<size_t>(total_len);
    return std::move(document);
}

void BsonCodec::write_int32(std::string& out, int32_t value)
{
    write_int32_le(out, value);
}

void BsonCodec::write_int64(std::string& out, int64_t value)
{
    write_int64_le(out, value);
}

void BsonCodec::write_double(std::string& out, double value)
{
    write_double_le(out, value);
}

std::expected<void, std::string> BsonCodec::write_c_string(std::string& out, std::string_view value)
{
    if (value.find('\0') != std::string::npos) {
        return std::unexpected("BSON CString must not contain embedded NUL bytes");
    }
    out.append(value.data(), value.size());
    out.push_back('\0');
    return {};
}

std::expected<int32_t, std::string> BsonCodec::read_int32(const char* data, size_t len, size_t pos)
{
    if (pos + 4 > len) {
        return std::unexpected("readInt32 out of range");
    }
    return read_int32_le_unchecked(data + pos);
}

std::expected<int64_t, std::string> BsonCodec::read_int64(const char* data, size_t len, size_t pos)
{
    if (pos + 8 > len) {
        return std::unexpected("readInt64 out of range");
    }
    return read_int64_le_unchecked(data + pos);
}

std::expected<double, std::string> BsonCodec::read_double(const char* data, size_t len, size_t pos)
{
    auto bits_or_err = read_int64(data, len, pos);
    if (!bits_or_err) {
        return std::unexpected(bits_or_err.error());
    }

    const uint64_t bits = static_cast<uint64_t>(bits_or_err.value());
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::expected<std::string, std::string> BsonCodec::read_c_string(const char* data, size_t len, size_t& pos)
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

std::expected<void, std::string> BsonCodec::encode_element(std::string& out,
                                                          std::string_view key,
                                                          const MongoValue& value)
{
    switch (value.type()) {
    case MongoValueType::Double:
        out.push_back(static_cast<char>(BsonType::Double));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        write_double(out, value.to_double());
        break;
    case MongoValueType::String: {
        out.push_back(static_cast<char>(BsonType::String));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        const auto& text = value.to_string();
        write_int32(out, static_cast<int32_t>(text.size() + 1));
        out.append(text);
        out.push_back('\0');
        break;
    }
    case MongoValueType::Document: {
        out.push_back(static_cast<char>(BsonType::Document));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        if (auto appended = append_document(out, value.to_document()); !appended) {
            return std::unexpected(appended.error());
        }
        break;
    }
    case MongoValueType::Array: {
        out.push_back(static_cast<char>(BsonType::Array));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        const size_t base = out.size();
        out.resize(base + 4, '\0');
        const auto& values = value.to_array().values();
        for (size_t i = 0; i < values.size(); ++i) {
            std::array<char, 24> key_buf{};
            const auto result =
                std::to_chars(key_buf.data(), key_buf.data() + key_buf.size(), i);
            if (result.ec != std::errc()) {
                return std::unexpected("failed to encode BSON array index");
            }
            const std::string_view index_key(
                key_buf.data(),
                static_cast<size_t>(result.ptr - key_buf.data()));
            if (auto encoded = encode_element(out, index_key, values[i]); !encoded) {
                return std::unexpected(encoded.error());
            }
        }
        out.push_back('\0');
        const auto total_len = static_cast<int32_t>(out.size() - base);
        write_int32_le_at(out, base, total_len);
        break;
    }
    case MongoValueType::Binary: {
        out.push_back(static_cast<char>(BsonType::Binary));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        const auto& binary = value.to_binary();
        write_int32(out, static_cast<int32_t>(binary.size()));
        out.push_back(static_cast<char>(kBinarySubtypeGeneric));
        out.append(reinterpret_cast<const char*>(binary.data()), binary.size());
        break;
    }
    case MongoValueType::Bool:
        out.push_back(static_cast<char>(BsonType::Bool));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        out.push_back(value.to_bool(false) ? 1 : 0);
        break;
    case MongoValueType::Null:
        out.push_back(static_cast<char>(BsonType::Null));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        break;
    case MongoValueType::Int32:
        out.push_back(static_cast<char>(BsonType::Int32));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        write_int32(out, value.to_int32());
        break;
    case MongoValueType::Int64:
        out.push_back(static_cast<char>(BsonType::Int64));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        write_int64(out, value.to_int64());
        break;
    case MongoValueType::ObjectId: {
        out.push_back(static_cast<char>(BsonType::ObjectId));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        // decode 24-char hex string back to 12 raw bytes
        const auto& oid_hex = value.to_string();
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
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        write_int64(out, value.to_int64());
        break;
    case MongoValueType::Timestamp:
        out.push_back(static_cast<char>(BsonType::Timestamp));
        if (auto written = write_c_string(out, key); !written) return std::unexpected(written.error());
        write_int64(out, value.to_int64());
        break;
    }
    return {};
}

std::expected<MongoValue, std::string> BsonCodec::decode_element_value(BsonType type,
                                                                      const char* data,
                                                                      size_t len,
                                                                      size_t& pos)
{
    switch (type) {
    case BsonType::Double: {
        auto d = read_double(data, len, pos);
        if (!d) return std::unexpected(d.error());
        pos += 8;
        return MongoValue(d.value());
    }
    case BsonType::String: {
        auto str_len_or_err = read_int32(data, len, pos);
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
        auto doc_or_err = decode_document(data + pos, len - pos, consumed);
        if (!doc_or_err) {
            return std::unexpected(doc_or_err.error());
        }
        pos += consumed;
        return MongoValue(std::move(doc_or_err.value()));
    }
    case BsonType::Array: {
        size_t consumed = 0;
        auto doc_or_err = decode_document(data + pos, len - pos, consumed);
        if (!doc_or_err) {
            return std::unexpected(doc_or_err.error());
        }
        pos += consumed;
        return MongoValue(decode_array_from_document(std::move(doc_or_err.value())));
    }
    case BsonType::Binary: {
        auto blob_len_or_err = read_int32(data, len, pos);
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
        // encode 12 raw bytes as 24-char hex string
        static constexpr char hex_chars[] = "0123456789abcdef";
        std::string oid;
        oid.reserve(24);
        for (size_t i = 0; i < 12; ++i) {
            const auto byte = static_cast<uint8_t>(data[pos + i]);
            oid.push_back(hex_chars[byte >> 4]);
            oid.push_back(hex_chars[byte & 0x0F]);
        }
        pos += 12;
        return MongoValue::from_object_id(std::move(oid));
    }
    case BsonType::Bool: {
        if (pos + 1 > len) {
            return std::unexpected("BSON bool out of range");
        }
        const bool value = data[pos++] != 0;
        return MongoValue(value);
    }
    case BsonType::DateTime: {
        auto ts = read_int64(data, len, pos);
        if (!ts) return std::unexpected(ts.error());
        pos += 8;
        return MongoValue::from_date_time(ts.value());
    }
    case BsonType::Null:
        return MongoValue(nullptr);
    case BsonType::Int32: {
        auto i32 = read_int32(data, len, pos);
        if (!i32) return std::unexpected(i32.error());
        pos += 4;
        return MongoValue(i32.value());
    }
    case BsonType::Timestamp: {
        auto ts = read_int64(data, len, pos);
        if (!ts) return std::unexpected(ts.error());
        pos += 8;
        return MongoValue::from_timestamp(static_cast<uint64_t>(ts.value()));
    }
    case BsonType::Int64: {
        auto i64 = read_int64(data, len, pos);
        if (!i64) return std::unexpected(i64.error());
        pos += 8;
        return MongoValue(i64.value());
    }
    default:
        return std::unexpected("Unsupported BSON type: " + std::to_string(static_cast<int>(type)));
    }
}

} // namespace galay::mongo::protocol
