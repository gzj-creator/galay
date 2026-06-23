#include "mongo.h"

#include "../../cpp/galay-mongo/base/mongo_uri.h"
#include "../../cpp/galay-mongo/protoc/bson.h"
#include "../../cpp/galay-mongo/sync/mongo_client.h"

#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>

struct galay_mongo_document {
    galay::mongo::MongoDocument document;
    std::string encoded;
};

struct galay_mongo_uri {
    galay::mongo::MongoConfig config;
};

struct galay_mongo_client {
    galay::mongo::MongoClient client;
};

namespace {

size_t bounded_c_string_length(const char* value, size_t max_len, bool* terminated) noexcept
{
    if (value == nullptr) {
        if (terminated != nullptr) {
            *terminated = false;
        }
        return 0;
    }
    for (size_t i = 0; i <= max_len; ++i) {
        if (value[i] == '\0') {
            if (terminated != nullptr) {
                *terminated = true;
            }
            return i;
        }
    }
    if (terminated != nullptr) {
        *terminated = false;
    }
    return max_len + 1;
}

galay_status_t validate_key(const char* key, std::string_view* out) noexcept
{
    bool terminated = false;
    const size_t len = bounded_c_string_length(key, GALAY_MONGO_MAX_KEY_LENGTH, &terminated);
    if (key == nullptr || !terminated || len == 0 || len > GALAY_MONGO_MAX_KEY_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = std::string_view(key, len);
    return GALAY_OK;
}

bool uri_has_database(std::string_view uri) noexcept
{
    constexpr std::string_view scheme = "mongodb://";
    if (!uri.starts_with(scheme)) {
        return false;
    }
    const std::string_view rest = uri.substr(scheme.size());
    const size_t query = rest.find('?');
    const std::string_view before_query =
        query == std::string_view::npos ? rest : rest.substr(0, query);
    const size_t slash = before_query.find('/');
    return slash != std::string_view::npos && slash + 1 < before_query.size();
}

galay_status_t map_mongo_error(const galay::mongo::MongoError& error) noexcept
{
    using namespace galay::mongo;
    switch (error.type()) {
    case MONGO_ERROR_SUCCESS:
        return GALAY_OK;
    case MONGO_ERROR_CONNECTION:
    case MONGO_ERROR_SEND:
    case MONGO_ERROR_RECV:
    case MONGO_ERROR_CONNECTION_CLOSED:
    case MONGO_ERROR_TIMEOUT:
        return GALAY_IO_ERROR;
    case MONGO_ERROR_PROTOCOL:
    case MONGO_ERROR_COMMAND:
    case MONGO_ERROR_SERVER:
        return GALAY_PROTOCOL_ERROR;
    case MONGO_ERROR_INVALID_PARAM:
        return GALAY_INVALID_ARGUMENT;
    case MONGO_ERROR_UNSUPPORTED:
        return GALAY_UNSUPPORTED;
    case MONGO_ERROR_BUFFER_OVERFLOW:
        return GALAY_OUT_OF_MEMORY;
    case MONGO_ERROR_AUTH:
    case MONGO_ERROR_INTERNAL:
    default:
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t append_value(galay_mongo_document_t* document,
                            const char* key,
                            galay::mongo::MongoValue value) noexcept
{
    if (document == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string_view key_view;
    const auto key_status = validate_key(key, &key_view);
    if (key_status != GALAY_OK) {
        return key_status;
    }
    document->document.append(std::string(key_view), std::move(value));
    document->encoded.clear();
    return GALAY_OK;
}

const galay::mongo::MongoValue* find_value(const galay_mongo_document_t* document,
                                           const char* key,
                                           galay_status_t* status) noexcept
{
    if (status != nullptr) {
        *status = GALAY_OK;
    }
    if (document == nullptr) {
        if (status != nullptr) {
            *status = GALAY_INVALID_ARGUMENT;
        }
        return nullptr;
    }
    std::string_view key_view;
    const auto key_status = validate_key(key, &key_view);
    if (key_status != GALAY_OK) {
        if (status != nullptr) {
            *status = key_status;
        }
        return nullptr;
    }
    const auto* value = document->document.find(std::string(key_view));
    if (value == nullptr && status != nullptr) {
        *status = GALAY_NOT_FOUND;
    }
    return value;
}

galay_status_t copy_string_view(const std::string& input,
                                const char** value,
                                size_t* value_len) noexcept
{
    if (value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = input.data();
    *value_len = input.size();
    return GALAY_OK;
}

} // namespace

extern "C" {

galay_status_t galay_mongo_document_create(galay_mongo_document_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    *out = new (std::nothrow) galay_mongo_document{};
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_document_destroy(galay_mongo_document_t* document)
{
    delete document;
}

size_t galay_mongo_document_size(const galay_mongo_document_t* document)
{
    return document == nullptr ? 0 : document->document.size();
}

galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document,
                                                 const char* key,
                                                 int32_t value)
{
    return append_value(document, key, galay::mongo::MongoValue(value));
}

galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document,
                                                 const char* key,
                                                 int64_t value)
{
    return append_value(document, key, galay::mongo::MongoValue(value));
}

galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document,
                                                  const char* key,
                                                  double value)
{
    return append_value(document, key, galay::mongo::MongoValue(value));
}

galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document,
                                                const char* key,
                                                galay_bool_t value)
{
    return append_value(document, key, galay::mongo::MongoValue(value != GALAY_FALSE));
}

galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document,
                                                  const char* key,
                                                  const char* value,
                                                  size_t value_len)
{
    if (value == nullptr && value_len != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (value_len > GALAY_MONGO_MAX_STRING_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    return append_value(document, key, galay::mongo::MongoValue(
        std::string(value == nullptr ? "" : value, value_len)));
}

galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document,
                                                const char* key)
{
    return append_value(document, key, galay::mongo::MongoValue(nullptr));
}

galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document,
                                              const char* key,
                                              int32_t* value)
{
    if (value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_status_t status = GALAY_OK;
    const auto* found = find_value(document, key, &status);
    if (found == nullptr) {
        return status;
    }
    *value = found->toInt32();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document,
                                              const char* key,
                                              int64_t* value)
{
    if (value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_status_t status = GALAY_OK;
    const auto* found = find_value(document, key, &status);
    if (found == nullptr) {
        return status;
    }
    *value = found->toInt64();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document,
                                               const char* key,
                                               double* value)
{
    if (value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_status_t status = GALAY_OK;
    const auto* found = find_value(document, key, &status);
    if (found == nullptr) {
        return status;
    }
    *value = found->toDouble();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document,
                                             const char* key,
                                             galay_bool_t* value)
{
    if (value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_status_t status = GALAY_OK;
    const auto* found = find_value(document, key, &status);
    if (found == nullptr) {
        return status;
    }
    *value = found->toBool(false) ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document,
                                               const char* key,
                                               const char** value,
                                               size_t* value_len)
{
    if (value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *value = nullptr;
    *value_len = 0;
    galay_status_t status = GALAY_OK;
    const auto* found = find_value(document, key, &status);
    if (found == nullptr) {
        return status;
    }
    if (!found->isString()) {
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_view(found->toString(), value, value_len);
}

galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document,
                                           const uint8_t** data,
                                           size_t* data_len)
{
    if (document == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *data = nullptr;
    *data_len = 0;
    auto encoded = galay::mongo::protocol::BsonCodec::encodeDocument(document->document);
    if (!encoded) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->encoded = std::move(encoded.value());
    *data = reinterpret_cast<const uint8_t*>(document->encoded.data());
    *data_len = document->encoded.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_decode(const uint8_t* data,
                                           size_t data_len,
                                           galay_mongo_document_t** out)
{
    if ((data == nullptr && data_len != 0) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    auto decoded = galay::mongo::protocol::BsonCodec::decodeDocument(
        reinterpret_cast<const char*>(data),
        data_len);
    if (!decoded) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* handle = new (std::nothrow) galay_mongo_document{};
    if (handle == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    handle->document = std::move(decoded.value());
    *out = handle;
    return GALAY_OK;
}

galay_status_t galay_mongo_uri_parse(const char* uri, galay_mongo_uri_t** out)
{
    if (uri == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    const std::string_view uri_view(uri);
    if (!uri_has_database(uri_view)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto parsed = galay::mongo::parseMongoUri(uri_view);
    if (!parsed) {
        return map_mongo_error(parsed.error());
    }
    auto* handle = new (std::nothrow) galay_mongo_uri{};
    if (handle == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    handle->config = std::move(parsed.value());
    *out = handle;
    return GALAY_OK;
}

void galay_mongo_uri_destroy(galay_mongo_uri_t* uri)
{
    delete uri;
}

galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri,
                                    const char** value,
                                    size_t* value_len)
{
    if (uri == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_view(uri->config.host, value, value_len);
}

galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri,
                                        const char** value,
                                        size_t* value_len)
{
    if (uri == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_view(uri->config.database, value, value_len);
}

galay_status_t galay_mongo_uri_port(const galay_mongo_uri_t* uri, uint16_t* port)
{
    if (uri == nullptr || port == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *port = uri->config.port;
    return GALAY_OK;
}

galay_status_t galay_mongo_client_create(galay_mongo_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;
    *out = new (std::nothrow) galay_mongo_client{};
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_client_destroy(galay_mongo_client_t* client)
{
    delete client;
}

galay_status_t galay_mongo_client_connect_uri(galay_mongo_client_t* client, const char* uri)
{
    if (client == nullptr || uri == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string_view uri_view(uri);
    if (!uri_has_database(uri_view)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto parsed = galay::mongo::parseMongoUri(uri_view);
    if (!parsed) {
        return map_mongo_error(parsed.error());
    }
    auto connected = client->client.connect(parsed.value());
    if (!connected) {
        return map_mongo_error(connected.error());
    }
    return GALAY_OK;
}

galay_status_t galay_mongo_client_ping(galay_mongo_client_t* client, const char* database)
{
    if (client == nullptr || database == nullptr || database[0] == '\0' ||
        !client->client.isConnected()) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto result = client->client.ping(database);
    if (!result) {
        return map_mongo_error(result.error());
    }
    return GALAY_OK;
}

void galay_mongo_client_close(galay_mongo_client_t* client)
{
    if (client != nullptr) {
        client->client.close();
    }
}

galay_bool_t galay_mongo_client_is_connected(const galay_mongo_client_t* client)
{
    return client != nullptr && client->client.isConnected() ? GALAY_TRUE : GALAY_FALSE;
}

} // extern "C"
