#include <galay/cpp/galay-mcp/common/mcp_json.h>
#include <charconv>
#include <cstdio>
#include <functional>
#include <utility>

namespace galay {
namespace mcp {

std::expected<JsonDocument, McpError> JsonDocument::Parse(std::string_view json) {
    JsonDocument doc;
    try {
        doc.m_parser = std::make_unique<simdjson::dom::parser>();
        doc.m_buffer = simdjson::padded_string(json);
        auto parsed = doc.m_parser->parse(doc.m_buffer);
        if (parsed.error()) {
            return std::unexpected(McpError::parseError(simdjson::error_message(parsed.error())));
        }
        doc.m_root = parsed.value();
        return doc;
    } catch (const std::exception& e) {
        return std::unexpected(McpError::parseError(e.what()));
    }
}

void JsonWriter::StartObject() {
    WriteValuePrefix();
    m_out.push_back('{');
    m_stack.push_back({ContextType::Object, true, false});
}

void JsonWriter::EndObject() {
    m_out.push_back('}');
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

void JsonWriter::StartArray() {
    WriteValuePrefix();
    m_out.push_back('[');
    m_stack.push_back({ContextType::Array, true, false});
}

void JsonWriter::EndArray() {
    m_out.push_back(']');
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

void JsonWriter::Key(const std::string& key) {
    if (m_stack.empty() || m_stack.back().type != ContextType::Object) {
        return;
    }

    if (!m_stack.back().first) {
        m_out.push_back(',');
    }
    m_stack.back().first = false;
    m_stack.back().expectValue = true;

    m_out.push_back('\"');
    AppendEscaped(m_out, key);
    m_out.append("\":");
}

void JsonWriter::String(const std::string& value) {
    WriteValuePrefix();
    m_out.push_back('\"');
    AppendEscaped(m_out, value);
    m_out.push_back('\"');
}

void JsonWriter::Number(int64_t value) {
    WriteValuePrefix();
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc()) {
        m_out.append(buf, ptr);
    }
}

void JsonWriter::Number(uint64_t value) {
    WriteValuePrefix();
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc()) {
        m_out.append(buf, ptr);
    }
}

void JsonWriter::Number(double value) {
    WriteValuePrefix();
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc()) {
        m_out.append(buf, ptr);
    } else {
        m_out.append("0");
    }
}

void JsonWriter::Bool(bool value) {
    WriteValuePrefix();
    m_out.append(value ? "true" : "false");
}

void JsonWriter::Null() {
    WriteValuePrefix();
    m_out.append("null");
}

void JsonWriter::Raw(const std::string& json) {
    WriteValuePrefix();
    m_out.append(json);
}

std::string JsonWriter::TakeString() {
    return std::move(m_out);
}

void JsonWriter::WriteValuePrefix() {
    if (m_stack.empty()) {
        return;
    }

    auto& ctx = m_stack.back();
    if (ctx.type == ContextType::Object) {
        if (!ctx.expectValue) {
            return;
        }
        ctx.expectValue = false;
    } else {
        if (!ctx.first) {
            m_out.push_back(',');
        }
        ctx.first = false;
    }
}

void JsonWriter::AppendEscaped(std::string& out, const std::string& value) {
    for (char c : value) {
        switch (c) {
            case '\"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
        }
    }
}

bool JsonHelper::GetObject(const JsonElement& element, JsonObject& out) {
    auto obj = element.get_object();
    if (obj.error()) {
        return false;
    }
    out = obj.value();
    return true;
}

bool JsonHelper::GetArray(const JsonElement& element, JsonArray& out) {
    auto arr = element.get_array();
    if (arr.error()) {
        return false;
    }
    out = arr.value();
    return true;
}

bool JsonHelper::GetStringValue(const JsonElement& element, std::string& out) {
    auto str = element.get_string();
    if (str.error()) {
        return false;
    }
    out = std::string(str.value());
    return true;
}

bool JsonHelper::GetRawJson(const JsonElement& element, std::string& out) {
    JsonWriter writer;
    std::function<bool(const JsonElement&)> writeElement = [&](const JsonElement& value) -> bool {
        switch (value.type()) {
            case simdjson::dom::element_type::ARRAY: {
                auto arr = value.get_array();
                if (arr.error()) {
                    return false;
                }
                writer.StartArray();
                for (auto item : arr.value()) {
                    if (!writeElement(item)) {
                        return false;
                    }
                }
                writer.EndArray();
                return true;
            }
            case simdjson::dom::element_type::OBJECT: {
                auto obj = value.get_object();
                if (obj.error()) {
                    return false;
                }
                writer.StartObject();
                for (auto field : obj.value()) {
                    writer.Key(std::string(field.key));
                    if (!writeElement(field.value)) {
                        return false;
                    }
                }
                writer.EndObject();
                return true;
            }
            case simdjson::dom::element_type::STRING: {
                auto str = value.get_string();
                if (str.error()) {
                    return false;
                }
                writer.String(std::string(str.value()));
                return true;
            }
            case simdjson::dom::element_type::INT64: {
                auto num = value.get_int64();
                if (num.error()) {
                    return false;
                }
                writer.Number(num.value());
                return true;
            }
            case simdjson::dom::element_type::UINT64: {
                auto num = value.get_uint64();
                if (num.error()) {
                    return false;
                }
                writer.Number(num.value());
                return true;
            }
            case simdjson::dom::element_type::DOUBLE: {
                auto num = value.get_double();
                if (num.error()) {
                    return false;
                }
                writer.Number(num.value());
                return true;
            }
            case simdjson::dom::element_type::BOOL: {
                auto b = value.get_bool();
                if (b.error()) {
                    return false;
                }
                writer.Bool(b.value());
                return true;
            }
            case simdjson::dom::element_type::NULL_VALUE: {
                writer.Null();
                return true;
            }
        }
        return false;
    };

    if (!writeElement(element)) {
        return false;
    }
    out = writer.TakeString();
    return true;
}

bool JsonHelper::GetString(const JsonObject& obj, const char* key, std::string& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    return GetStringValue(val.value(), out);
}

bool JsonHelper::GetInt64(const JsonObject& obj, const char* key, int64_t& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    auto num = val.value().get_int64();
    if (num.error()) {
        return false;
    }
    out = num.value();
    return true;
}

bool JsonHelper::GetBool(const JsonObject& obj, const char* key, bool& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    auto b = val.value().get_bool();
    if (b.error()) {
        return false;
    }
    out = b.value();
    return true;
}

bool JsonHelper::GetElement(const JsonObject& obj, const char* key, JsonElement& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    out = val.value();
    return true;
}

bool JsonHelper::GetObject(const JsonObject& obj, const char* key, JsonObject& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    return GetObject(val.value(), out);
}

bool JsonHelper::GetArray(const JsonObject& obj, const char* key, JsonArray& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    return GetArray(val.value(), out);
}

const JsonElement& JsonHelper::EmptyObject() {
    static JsonDocument emptyDoc = []() {
        auto parsed = JsonDocument::Parse("{}");
        if (!parsed) {
            return JsonDocument{};
        }
        return std::move(parsed.value());
    }();
    return emptyDoc.Root();
}

} // namespace mcp
} // namespace galay
