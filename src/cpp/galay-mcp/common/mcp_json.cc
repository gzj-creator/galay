#include <galay/cpp/galay-mcp/common/mcp_json.h>
#include <charconv>
#include <cstdio>
#include <functional>
#include <utility>

namespace galay {
namespace mcp {

std::expected<JsonDocument, McpError> JsonDocument::parse(std::string_view json) {
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

void JsonWriter::startObject() {
    writeValuePrefix();
    m_out.push_back('{');
    m_stack.push_back({ContextType::Object, true, false});
}

void JsonWriter::endObject() {
    m_out.push_back('}');
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

void JsonWriter::startArray() {
    writeValuePrefix();
    m_out.push_back('[');
    m_stack.push_back({ContextType::Array, true, false});
}

void JsonWriter::endArray() {
    m_out.push_back(']');
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
}

void JsonWriter::key(const std::string& key) {
    if (m_stack.empty() || m_stack.back().type != ContextType::Object) {
        return;
    }

    if (!m_stack.back().first) {
        m_out.push_back(',');
    }
    m_stack.back().first = false;
    m_stack.back().expectValue = true;

    m_out.push_back('\"');
    appendEscaped(m_out, key);
    m_out.append("\":");
}

void JsonWriter::string(const std::string& value) {
    writeValuePrefix();
    m_out.push_back('\"');
    appendEscaped(m_out, value);
    m_out.push_back('\"');
}

void JsonWriter::number(int64_t value) {
    writeValuePrefix();
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc()) {
        m_out.append(buf, ptr);
    }
}

void JsonWriter::number(uint64_t value) {
    writeValuePrefix();
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc()) {
        m_out.append(buf, ptr);
    }
}

void JsonWriter::number(double value) {
    writeValuePrefix();
    char buf[64];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    if (ec == std::errc()) {
        m_out.append(buf, ptr);
    } else {
        m_out.append("0");
    }
}

void JsonWriter::boolean(bool value) {
    writeValuePrefix();
    m_out.append(value ? "true" : "false");
}

void JsonWriter::nullValue() {
    writeValuePrefix();
    m_out.append("null");
}

void JsonWriter::raw(const std::string& json) {
    writeValuePrefix();
    m_out.append(json);
}

std::string JsonWriter::takeString() {
    return std::move(m_out);
}

void JsonWriter::writeValuePrefix() {
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

void JsonWriter::appendEscaped(std::string& out, const std::string& value) {
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

bool JsonHelper::getObject(const JsonElement& element, JsonObject& out) {
    auto obj = element.get_object();
    if (obj.error()) {
        return false;
    }
    out = obj.value();
    return true;
}

bool JsonHelper::getArray(const JsonElement& element, JsonArray& out) {
    auto arr = element.get_array();
    if (arr.error()) {
        return false;
    }
    out = arr.value();
    return true;
}

bool JsonHelper::getStringValue(const JsonElement& element, std::string& out) {
    auto str = element.get_string();
    if (str.error()) {
        return false;
    }
    out = std::string(str.value());
    return true;
}

bool JsonHelper::getRawJson(const JsonElement& element, std::string& out) {
    JsonWriter writer;
    std::function<bool(const JsonElement&)> writeElement = [&](const JsonElement& value) -> bool {
        switch (value.type()) {
            case simdjson::dom::element_type::ARRAY: {
                auto arr = value.get_array();
                if (arr.error()) {
                    return false;
                }
                writer.startArray();
                for (auto item : arr.value()) {
                    if (!writeElement(item)) {
                        return false;
                    }
                }
                writer.endArray();
                return true;
            }
            case simdjson::dom::element_type::OBJECT: {
                auto obj = value.get_object();
                if (obj.error()) {
                    return false;
                }
                writer.startObject();
                for (auto field : obj.value()) {
                    writer.key(std::string(field.key));
                    if (!writeElement(field.value)) {
                        return false;
                    }
                }
                writer.endObject();
                return true;
            }
            case simdjson::dom::element_type::STRING: {
                auto str = value.get_string();
                if (str.error()) {
                    return false;
                }
                writer.string(std::string(str.value()));
                return true;
            }
            case simdjson::dom::element_type::INT64: {
                auto num = value.get_int64();
                if (num.error()) {
                    return false;
                }
                writer.number(num.value());
                return true;
            }
            case simdjson::dom::element_type::UINT64: {
                auto num = value.get_uint64();
                if (num.error()) {
                    return false;
                }
                writer.number(num.value());
                return true;
            }
            case simdjson::dom::element_type::DOUBLE: {
                auto num = value.get_double();
                if (num.error()) {
                    return false;
                }
                writer.number(num.value());
                return true;
            }
            case simdjson::dom::element_type::BOOL: {
                auto b = value.get_bool();
                if (b.error()) {
                    return false;
                }
                writer.boolean(b.value());
                return true;
            }
            case simdjson::dom::element_type::NULL_VALUE: {
                writer.nullValue();
                return true;
            }
        }
        return false;
    };

    if (!writeElement(element)) {
        return false;
    }
    out = writer.takeString();
    return true;
}

bool JsonHelper::getString(const JsonObject& obj, const char* key, std::string& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    return getStringValue(val.value(), out);
}

bool JsonHelper::getInt64(const JsonObject& obj, const char* key, int64_t& out) {
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

bool JsonHelper::getBool(const JsonObject& obj, const char* key, bool& out) {
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

bool JsonHelper::getElement(const JsonObject& obj, const char* key, JsonElement& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    out = val.value();
    return true;
}

bool JsonHelper::getObject(const JsonObject& obj, const char* key, JsonObject& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    return getObject(val.value(), out);
}

bool JsonHelper::getArray(const JsonObject& obj, const char* key, JsonArray& out) {
    auto val = obj[key];
    if (val.error()) {
        return false;
    }
    return getArray(val.value(), out);
}

const JsonElement& JsonHelper::emptyObject() {
    static JsonDocument emptyDoc = []() {
        auto parsed = JsonDocument::parse("{}");
        if (!parsed) {
            return JsonDocument{};
        }
        return std::move(parsed.value());
    }();
    return emptyDoc.root();
}

} // namespace mcp
} // namespace galay
