#include <galay/c/galay-mongo-c/mongo.h>

#include <cstring>
#include <new>
#include <string>
#include <variant>
#include <vector>

using mongo_value = std::variant<int32_t, int64_t, double, bool, std::string, std::monostate>;

struct mongo_item {
    std::string key;
    mongo_value value;
};

struct galay_mongo_document_t {
    std::vector<mongo_item> entries;
    std::vector<uint8_t> encoded;
};

struct galay_mongo_uri_t {
    std::string host;
    std::string database;
    uint16_t port = 27017;
};

struct galay_mongo_client_t {
    bool connected = false;
};

static galay_status_t validate_key(const char* key)
{
    if (key == nullptr || key[0] == '\0' || std::strlen(key) > GALAY_MONGO_MAX_KEY_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

static const mongo_item* find_entry_const(const galay_mongo_document_t* document, const char* key)
{
    for (const auto& entry : document->entries) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

extern "C" {

const char* galay_mongo_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_mongo_document_create(galay_mongo_document_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mongo_document_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_document_destroy(galay_mongo_document_t* document)
{
    delete document;
}

size_t galay_mongo_document_size(const galay_mongo_document_t* document)
{
    return document == nullptr ? 0 : document->entries.size();
}

galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document, const char* key, int32_t value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->entries.push_back({key, value});
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document, const char* key, int64_t value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->entries.push_back({key, value});
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document, const char* key, double value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->entries.push_back({key, value});
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document, const char* key, galay_bool_t value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->entries.push_back({key, value == GALAY_TRUE});
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document, const char* key, const char* value, size_t value_len)
{
    if (document == nullptr || validate_key(key) != GALAY_OK ||
        (value == nullptr && value_len != 0) || value_len > GALAY_MONGO_MAX_STRING_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->entries.push_back({key, std::string(value == nullptr ? "" : value, value_len)});
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document, const char* key)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->entries.push_back({key, std::monostate{}});
    return GALAY_OK;
}

galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document, const uint8_t** bson, size_t* bson_len)
{
    if (document == nullptr || bson == nullptr || bson_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->encoded.clear();
    document->encoded.push_back('G');
    document->encoded.push_back('M');
    document->encoded.push_back('D');
    document->encoded.push_back('1');
    document->encoded.push_back(static_cast<uint8_t>(document->entries.size()));
    for (const auto& entry : document->entries) {
        document->encoded.push_back(static_cast<uint8_t>(entry.key.size()));
        document->encoded.insert(document->encoded.end(), entry.key.begin(), entry.key.end());
        if (std::holds_alternative<int32_t>(entry.value)) {
            document->encoded.push_back(1);
            const int32_t value = std::get<int32_t>(entry.value);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            document->encoded.insert(document->encoded.end(), bytes, bytes + sizeof(value));
        } else if (std::holds_alternative<int64_t>(entry.value)) {
            document->encoded.push_back(2);
            const int64_t value = std::get<int64_t>(entry.value);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            document->encoded.insert(document->encoded.end(), bytes, bytes + sizeof(value));
        } else if (std::holds_alternative<double>(entry.value)) {
            document->encoded.push_back(3);
            const double value = std::get<double>(entry.value);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
            document->encoded.insert(document->encoded.end(), bytes, bytes + sizeof(value));
        } else if (std::holds_alternative<bool>(entry.value)) {
            document->encoded.push_back(4);
            document->encoded.push_back(std::get<bool>(entry.value) ? 1 : 0);
        } else if (std::holds_alternative<std::string>(entry.value)) {
            document->encoded.push_back(5);
            const auto& value = std::get<std::string>(entry.value);
            document->encoded.push_back(static_cast<uint8_t>(value.size() & 0xFFU));
            document->encoded.push_back(static_cast<uint8_t>((value.size() >> 8U) & 0xFFU));
            document->encoded.insert(document->encoded.end(), value.begin(), value.end());
        } else {
            document->encoded.push_back(6);
        }
    }
    *bson = document->encoded.data();
    *bson_len = document->encoded.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_decode(const uint8_t* bson, size_t bson_len, galay_mongo_document_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (bson == nullptr || out == nullptr || bson_len < 5 || std::memcmp(bson, "GMD1", 4) != 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* document = new (std::nothrow) galay_mongo_document_t();
    if (document == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    size_t pos = 5;
    const size_t count = bson[4];
    for (size_t i = 0; i < count; ++i) {
        if (pos >= bson_len) {
            delete document;
            return GALAY_PROTOCOL_ERROR;
        }
        const size_t key_len = bson[pos++];
        if (pos + key_len + 1 > bson_len) {
            delete document;
            return GALAY_PROTOCOL_ERROR;
        }
        std::string key(reinterpret_cast<const char*>(bson + pos), key_len);
        pos += key_len;
        const uint8_t type = bson[pos++];
        if (type == 1 && pos + 4 <= bson_len) {
            int32_t value = 0;
            std::memcpy(&value, bson + pos, sizeof(value));
            pos += sizeof(value);
            document->entries.push_back({key, value});
        } else if (type == 2 && pos + 8 <= bson_len) {
            int64_t value = 0;
            std::memcpy(&value, bson + pos, sizeof(value));
            pos += sizeof(value);
            document->entries.push_back({key, value});
        } else if (type == 3 && pos + 8 <= bson_len) {
            double value = 0.0;
            std::memcpy(&value, bson + pos, sizeof(value));
            pos += sizeof(value);
            document->entries.push_back({key, value});
        } else if (type == 4 && pos + 1 <= bson_len) {
            document->entries.push_back({key, bson[pos++] != 0});
        } else if (type == 5 && pos + 2 <= bson_len) {
            const size_t value_len = bson[pos] | (static_cast<size_t>(bson[pos + 1]) << 8U);
            pos += 2;
            if (pos + value_len > bson_len) {
                delete document;
                return GALAY_PROTOCOL_ERROR;
            }
            document->entries.push_back({key, std::string(reinterpret_cast<const char*>(bson + pos), value_len)});
            pos += value_len;
        } else if (type == 6) {
            document->entries.push_back({key, std::monostate{}});
        } else {
            delete document;
            return GALAY_PROTOCOL_ERROR;
        }
    }
    *out = document;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document, const char* key, int32_t* value)
{
    const auto* entry = document == nullptr || key == nullptr ? nullptr : find_entry_const(document, key);
    if (entry == nullptr) return GALAY_NOT_FOUND;
    if (!std::holds_alternative<int32_t>(entry->value) || value == nullptr) return GALAY_INVALID_ARGUMENT;
    *value = std::get<int32_t>(entry->value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document, const char* key, int64_t* value)
{
    const auto* entry = document == nullptr || key == nullptr ? nullptr : find_entry_const(document, key);
    if (entry == nullptr) return GALAY_NOT_FOUND;
    if (!std::holds_alternative<int64_t>(entry->value) || value == nullptr) return GALAY_INVALID_ARGUMENT;
    *value = std::get<int64_t>(entry->value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document, const char* key, double* value)
{
    const auto* entry = document == nullptr || key == nullptr ? nullptr : find_entry_const(document, key);
    if (entry == nullptr) return GALAY_NOT_FOUND;
    if (!std::holds_alternative<double>(entry->value) || value == nullptr) return GALAY_INVALID_ARGUMENT;
    *value = std::get<double>(entry->value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document, const char* key, galay_bool_t* value)
{
    const auto* entry = document == nullptr || key == nullptr ? nullptr : find_entry_const(document, key);
    if (entry == nullptr) return GALAY_NOT_FOUND;
    if (!std::holds_alternative<bool>(entry->value) || value == nullptr) return GALAY_INVALID_ARGUMENT;
    *value = std::get<bool>(entry->value) ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    const auto* entry = document == nullptr || key == nullptr ? nullptr : find_entry_const(document, key);
    if (entry == nullptr) return GALAY_NOT_FOUND;
    if (!std::holds_alternative<std::string>(entry->value) || value == nullptr || value_len == nullptr) return GALAY_INVALID_ARGUMENT;
    const auto& text = std::get<std::string>(entry->value);
    *value = text.data();
    *value_len = text.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_uri_parse(const char* uri_text, galay_mongo_uri_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (uri_text == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    std::string text(uri_text);
    const std::string prefix = "mongodb://";
    if (text.rfind(prefix, 0) != 0) return GALAY_INVALID_ARGUMENT;
    const size_t slash = text.find('/', prefix.size());
    if (slash == std::string::npos || slash + 1 >= text.size()) return GALAY_INVALID_ARGUMENT;
    std::string host_port = text.substr(prefix.size(), slash - prefix.size());
    const size_t query = text.find('?', slash + 1);
    std::string db = text.substr(slash + 1, query == std::string::npos ? std::string::npos : query - slash - 1);
    if (db.empty()) return GALAY_INVALID_ARGUMENT;
    uint16_t port = 27017;
    size_t colon = host_port.find(':');
    std::string host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    if (host.empty()) return GALAY_INVALID_ARGUMENT;
    if (colon != std::string::npos) {
        const unsigned long parsed = std::strtoul(host_port.c_str() + colon + 1, nullptr, 10);
        if (parsed == 0 || parsed > 65535) return GALAY_INVALID_ARGUMENT;
        port = static_cast<uint16_t>(parsed);
    }
    auto* uri = new (std::nothrow) galay_mongo_uri_t();
    if (uri == nullptr) return GALAY_OUT_OF_MEMORY;
    uri->host = host;
    uri->database = db;
    uri->port = port;
    *out = uri;
    return GALAY_OK;
}

void galay_mongo_uri_destroy(galay_mongo_uri_t* uri)
{
    delete uri;
}

galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri, const char** host, size_t* host_len)
{
    if (uri == nullptr || host == nullptr || host_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *host = uri->host.data();
    *host_len = uri->host.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri, const char** database, size_t* database_len)
{
    if (uri == nullptr || database == nullptr || database_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *database = uri->database.data();
    *database_len = uri->database.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_uri_port(const galay_mongo_uri_t* uri, uint16_t* port)
{
    if (uri == nullptr || port == nullptr) return GALAY_INVALID_ARGUMENT;
    *port = uri->port;
    return GALAY_OK;
}

galay_status_t galay_mongo_client_create(galay_mongo_client_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_mongo_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_client_destroy(galay_mongo_client_t* client)
{
    delete client;
}

void galay_mongo_client_close(galay_mongo_client_t* client)
{
    if (client != nullptr) client->connected = false;
}

galay_bool_t galay_mongo_client_is_connected(const galay_mongo_client_t* client)
{
    return client != nullptr && client->connected ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_mongo_client_ping(galay_mongo_client_t* client, const char* database)
{
    if (client == nullptr || database == nullptr || !client->connected) return GALAY_INVALID_ARGUMENT;
    return GALAY_UNSUPPORTED;
}

}
