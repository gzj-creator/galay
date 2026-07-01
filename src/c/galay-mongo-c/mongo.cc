#include <galay/c/galay-mongo-c/mongo_c.h>

#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>
#include <galay/cpp/galay-mongo/protoc/bson.h>
#include <galay/cpp/galay-mongo/protoc/mongo_protocol.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr size_t kMongoMaxMessageSize = 16 * 1024 * 1024;

using galay::mongo::MongoArray;
using galay::mongo::MongoDocument;
using galay::mongo::MongoReply;
using galay::mongo::MongoValue;
using galay::mongo::MongoValueType;
using galay::mongo::protocol::BsonCodec;
using galay::mongo::protocol::MongoProtocol;

} // namespace

struct galay_mongo_document_t {
    MongoDocument document;
    std::vector<uint8_t> encoded;
};

struct galay_mongo_array_t {
    MongoArray array;
};

struct galay_mongo_uri_t {
    std::string host;
    std::string database;
    uint16_t port = 27017;
};

struct galay_mongo_client_t {
    std::string host = "127.0.0.1";
    uint16_t port = 27017;
    std::string database = "admin";
    galay_kernel_tcp_socket_t socket{};
    bool connected = false;
    int32_t next_request_id = 1;
    std::string recv_buffer;
};

namespace
{

C_IOResult make_io_result(C_IOResultCode code, int64_t value = 0)
{
    return C_IOResult{code, 0, 0, value, nullptr};
}

C_IOResult io_result_from_status(galay_status_t status)
{
    return make_io_result(status == GALAY_INVALID_ARGUMENT ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(status));
}

C_IOResult io_result_from_socket_create(C_TcpSocketResultCode code)
{
    return make_io_result(code == C_TcpSocketParameterInvalid ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(code));
}

galay_status_t validate_key(const char* key)
{
    if (key == nullptr || key[0] == '\0' || std::strlen(key) > GALAY_MONGO_MAX_KEY_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_status_t validate_text(const char* value, size_t value_len)
{
    if ((value == nullptr && value_len != 0) || value_len > GALAY_MONGO_MAX_STRING_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

const MongoValue* find_value(const galay_mongo_document_t* document, const char* key);
const MongoValue* array_value_at(const galay_mongo_array_t* array, size_t index);

galay_mongo_document_t* make_document(MongoDocument document)
{
    auto* out = new (std::nothrow) galay_mongo_document_t();
    if (out != nullptr) {
        out->document = std::move(document);
    }
    return out;
}

galay_mongo_array_t* make_array(MongoArray array)
{
    auto* out = new (std::nothrow) galay_mongo_array_t();
    if (out != nullptr) {
        out->array = std::move(array);
    }
    return out;
}

bool copy_host_to_c_host(const std::string& host, uint16_t port, C_Host* out)
{
    if (out == nullptr || host.empty() || host.size() >= sizeof(out->address) || port == 0) {
        return false;
    }
    out->type = host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memset(out->address, 0, sizeof(out->address));
    std::memcpy(out->address, host.data(), host.size());
    out->port = port;
    return true;
}

C_IOResult socket_read_exact(galay_kernel_tcp_socket_t* socket,
                             char* data,
                             size_t data_len,
                             int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    size_t received = 0;
    while (received < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_recv(socket,
                                                         data + received,
                                                         data_len - received,
                                                         timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        received += result.bytes;
    }

    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = received;
    return result;
}

C_IOResult socket_write_exact(galay_kernel_tcp_socket_t* socket,
                              const char* data,
                              size_t data_len,
                              int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket,
                                                         data + sent,
                                                         data_len - sent,
                                                         timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof);
        }
        sent += result.bytes;
    }

    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = sent;
    return result;
}

int32_t read_i32_le(const char* data)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint8_t>(data[0]))      ) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) <<  8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24));
}

C_IOResult read_mongo_message(galay_mongo_client_t* client,
                              int64_t timeout_ms,
                              galay::mongo::protocol::MongoMessage* message)
{
    if (client == nullptr || message == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    char header[16];
    C_IOResult header_result = socket_read_exact(&client->socket, header, sizeof(header), timeout_ms);
    if (header_result.code != C_IOResultOk) {
        return header_result;
    }

    const int32_t message_len = read_i32_le(header);
    if (message_len < 21 || static_cast<size_t>(message_len) > kMongoMaxMessageSize) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }

    client->recv_buffer.assign(header, sizeof(header));
    client->recv_buffer.resize(static_cast<size_t>(message_len));
    C_IOResult body_result = socket_read_exact(&client->socket,
                                               client->recv_buffer.data() + sizeof(header),
                                               static_cast<size_t>(message_len) - sizeof(header),
                                               timeout_ms);
    if (body_result.code != C_IOResultOk) {
        client->recv_buffer.clear();
        return body_result;
    }

    auto decoded = MongoProtocol::decodeMessage(client->recv_buffer.data(),
                                                client->recv_buffer.size());
    if (!decoded) {
        client->recv_buffer.clear();
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }

    *message = std::move(decoded.value());
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = client->recv_buffer.size();
    return result;
}

C_IOResult send_command(galay_mongo_client_t* client,
                        const char* database,
                        const MongoDocument& command,
                        int64_t timeout_ms,
                        galay_mongo_document_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || database == nullptr || reply == nullptr ||
        !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    const int32_t request_id = client->next_request_id++;
    if (client->next_request_id <= 0) {
        client->next_request_id = 1;
    }

    std::string encoded;
    auto appended = MongoProtocol::appendOpMsgWithDatabase(encoded,
                                                           request_id,
                                                           command,
                                                           database);
    if (!appended) {
        return io_result_from_status(GALAY_INVALID_ARGUMENT);
    }

    C_IOResult sent = socket_write_exact(&client->socket,
                                         encoded.data(),
                                         encoded.size(),
                                         timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }

    galay::mongo::protocol::MongoMessage message;
    C_IOResult received = read_mongo_message(client, timeout_ms, &message);
    if (received.code != C_IOResultOk) {
        return received;
    }
    if (message.header.response_to != request_id) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }

    MongoReply mongo_reply(message.body);
    if (!mongo_reply.ok()) {
        return make_io_result(C_IOResultError, static_cast<int64_t>(GALAY_PROTOCOL_ERROR));
    }

    auto* out = make_document(std::move(message.body));
    if (out == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }

    *reply = out;
    received.ptr = out;
    return received;
}

const MongoValue* find_value(const galay_mongo_document_t* document, const char* key)
{
    return document == nullptr || key == nullptr ? nullptr : document->document.find(key);
}

const MongoValue* array_value_at(const galay_mongo_array_t* array, size_t index)
{
    if (array == nullptr || index >= array->array.size()) {
        return nullptr;
    }
    return &array->array[index];
}

} // namespace

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
    return document == nullptr ? 0 : document->document.size();
}

galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document, const char* key, int32_t value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document, const char* key, int64_t value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document, const char* key, double value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document, const char* key, galay_bool_t value)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, value == GALAY_TRUE);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document, const char* key, const char* value, size_t value_len)
{
    if (document == nullptr || validate_key(key) != GALAY_OK ||
        validate_text(value, value_len) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, std::string(value == nullptr ? "" : value, value_len));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document, const char* key)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, nullptr);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_document(galay_mongo_document_t* document, const char* key, const galay_mongo_document_t* value)
{
    if (document == nullptr || value == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, value->document);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_array(galay_mongo_document_t* document, const char* key, const galay_mongo_array_t* value)
{
    if (document == nullptr || value == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, value->array);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_binary(galay_mongo_document_t* document, const char* key, const uint8_t* value, size_t value_len)
{
    if (document == nullptr || validate_key(key) != GALAY_OK || (value == nullptr && value_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    MongoValue::Binary binary;
    if (value_len != 0) {
        binary.assign(value, value + value_len);
    }
    document->document.append(key, MongoValue(std::move(binary)));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_object_id(galay_mongo_document_t* document, const char* key, const char* object_id_hex)
{
    if (document == nullptr || object_id_hex == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto value = MongoValue::fromObjectId(object_id_hex);
    if (!value) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, std::move(value.value()));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_date_time(galay_mongo_document_t* document, const char* key, int64_t millis)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, MongoValue::fromDateTime(millis));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_append_timestamp(galay_mongo_document_t* document, const char* key, uint64_t timestamp)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    document->document.append(key, MongoValue::fromTimestamp(timestamp));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document, const uint8_t** bson, size_t* bson_len)
{
    if (document == nullptr || bson == nullptr || bson_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto encoded = BsonCodec::encodeDocument(document->document);
    if (!encoded) {
        *bson = nullptr;
        *bson_len = 0;
        return GALAY_PROTOCOL_ERROR;
    }
    document->encoded.assign(encoded->begin(), encoded->end());
    *bson = document->encoded.data();
    *bson_len = document->encoded.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_decode(const uint8_t* bson, size_t bson_len, galay_mongo_document_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (bson == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto decoded = BsonCodec::decodeDocument(reinterpret_cast<const char*>(bson), bson_len);
    if (!decoded) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* document = make_document(std::move(decoded.value()));
    if (document == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    *out = document;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document, const char* key, int32_t* value)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Int32) return GALAY_INVALID_ARGUMENT;
    *value = found->toInt32();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document, const char* key, int64_t* value)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Int64) return GALAY_INVALID_ARGUMENT;
    *value = found->toInt64();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document, const char* key, double* value)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Double) return GALAY_INVALID_ARGUMENT;
    *value = found->toDouble();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document, const char* key, galay_bool_t* value)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Bool) return GALAY_INVALID_ARGUMENT;
    *value = found->toBool() ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || value_len == nullptr || found->type() != MongoValueType::String) return GALAY_INVALID_ARGUMENT;
    const auto& text = found->toString();
    *value = text.data();
    *value_len = text.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_document(const galay_mongo_document_t* document, const char* key, galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (out == nullptr || found->type() != MongoValueType::Document) return GALAY_INVALID_ARGUMENT;
    *out = make_document(found->toDocument());
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_document_get_array(const galay_mongo_document_t* document, const char* key, galay_mongo_array_t** out)
{
    if (out != nullptr) *out = nullptr;
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (out == nullptr || found->type() != MongoValueType::Array) return GALAY_INVALID_ARGUMENT;
    *out = make_array(found->toArray());
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_document_get_binary(const galay_mongo_document_t* document, const char* key, const uint8_t** value, size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || value_len == nullptr || found->type() != MongoValueType::Binary) return GALAY_INVALID_ARGUMENT;
    const auto& binary = found->toBinary();
    *value = binary.data();
    *value_len = binary.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_object_id(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || value_len == nullptr || found->type() != MongoValueType::ObjectId) return GALAY_INVALID_ARGUMENT;
    const auto& text = found->toString();
    *value = text.data();
    *value_len = text.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_date_time(const galay_mongo_document_t* document, const char* key, int64_t* value)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::DateTime) return GALAY_INVALID_ARGUMENT;
    *value = found->toInt64();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_timestamp(const galay_mongo_document_t* document, const char* key, uint64_t* value)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Timestamp) return GALAY_INVALID_ARGUMENT;
    *value = static_cast<uint64_t>(found->toInt64());
    return GALAY_OK;
}

galay_status_t galay_mongo_document_is_null(const galay_mongo_document_t* document, const char* key)
{
    const auto* found = find_value(document, key);
    if (found == nullptr) return GALAY_NOT_FOUND;
    return found->type() == MongoValueType::Null ? GALAY_OK : GALAY_INVALID_ARGUMENT;
}

galay_status_t galay_mongo_array_create(galay_mongo_array_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mongo_array_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_array_destroy(galay_mongo_array_t* array)
{
    delete array;
}

size_t galay_mongo_array_size(const galay_mongo_array_t* array)
{
    return array == nullptr ? 0 : array->array.size();
}

galay_status_t galay_mongo_array_append_int32(galay_mongo_array_t* array, int32_t value)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(value);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_int64(galay_mongo_array_t* array, int64_t value)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(value);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_double(galay_mongo_array_t* array, double value)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(value);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_bool(galay_mongo_array_t* array, galay_bool_t value)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(value == GALAY_TRUE);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_string(galay_mongo_array_t* array, const char* value, size_t value_len)
{
    if (array == nullptr || validate_text(value, value_len) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    array->array.append(std::string(value == nullptr ? "" : value, value_len));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_null(galay_mongo_array_t* array)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(nullptr);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_document(galay_mongo_array_t* array, const galay_mongo_document_t* value)
{
    if (array == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(value->document);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_array(galay_mongo_array_t* array, const galay_mongo_array_t* value)
{
    if (array == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(value->array);
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_binary(galay_mongo_array_t* array, const uint8_t* value, size_t value_len)
{
    if (array == nullptr || (value == nullptr && value_len != 0)) return GALAY_INVALID_ARGUMENT;
    MongoValue::Binary binary;
    if (value_len != 0) {
        binary.assign(value, value + value_len);
    }
    array->array.append(MongoValue(std::move(binary)));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_object_id(galay_mongo_array_t* array, const char* object_id_hex)
{
    if (array == nullptr || object_id_hex == nullptr) return GALAY_INVALID_ARGUMENT;
    auto value = MongoValue::fromObjectId(object_id_hex);
    if (!value) return GALAY_INVALID_ARGUMENT;
    array->array.append(std::move(value.value()));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_date_time(galay_mongo_array_t* array, int64_t millis)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(MongoValue::fromDateTime(millis));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_append_timestamp(galay_mongo_array_t* array, uint64_t timestamp)
{
    if (array == nullptr) return GALAY_INVALID_ARGUMENT;
    array->array.append(MongoValue::fromTimestamp(timestamp));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_int32(const galay_mongo_array_t* array, size_t index, int32_t* value)
{
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Int32) return GALAY_INVALID_ARGUMENT;
    *value = found->toInt32();
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_int64(const galay_mongo_array_t* array, size_t index, int64_t* value)
{
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Int64) return GALAY_INVALID_ARGUMENT;
    *value = found->toInt64();
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_double(const galay_mongo_array_t* array, size_t index, double* value)
{
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Double) return GALAY_INVALID_ARGUMENT;
    *value = found->toDouble();
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_bool(const galay_mongo_array_t* array, size_t index, galay_bool_t* value)
{
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || found->type() != MongoValueType::Bool) return GALAY_INVALID_ARGUMENT;
    *value = found->toBool() ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_string(const galay_mongo_array_t* array, size_t index, const char** value, size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (value == nullptr || value_len == nullptr || found->type() != MongoValueType::String) return GALAY_INVALID_ARGUMENT;
    const auto& text = found->toString();
    *value = text.data();
    *value_len = text.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_document(const galay_mongo_array_t* array, size_t index, galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (out == nullptr || found->type() != MongoValueType::Document) return GALAY_INVALID_ARGUMENT;
    *out = make_document(found->toDocument());
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_array_get_array(const galay_mongo_array_t* array, size_t index, galay_mongo_array_t** out)
{
    if (out != nullptr) *out = nullptr;
    const auto* found = array_value_at(array, index);
    if (found == nullptr) return GALAY_NOT_FOUND;
    if (out == nullptr || found->type() != MongoValueType::Array) return GALAY_INVALID_ARGUMENT;
    *out = make_array(found->toArray());
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
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
    const std::string host_port = text.substr(prefix.size(), slash - prefix.size());
    const size_t query = text.find('?', slash + 1);
    const std::string db = text.substr(slash + 1, query == std::string::npos ? std::string::npos : query - slash - 1);
    if (db.empty()) return GALAY_INVALID_ARGUMENT;
    uint16_t port = 27017;
    size_t colon = host_port.find(':');
    std::string host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    if (host.empty()) return GALAY_INVALID_ARGUMENT;
    if (colon != std::string::npos) {
        const char* port_start = host_port.c_str() + colon + 1;
        if (*port_start == '\0') return GALAY_INVALID_ARGUMENT;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(port_start, &end, 10);
        if (end == port_start || *end != '\0' || parsed == 0 || parsed > 65535) {
            return GALAY_INVALID_ARGUMENT;
        }
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

galay_status_t galay_mongo_command_find_one(const char* database, const char* collection,
                                            const galay_mongo_document_t* filter,
                                            const galay_mongo_document_t* projection,
                                            galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        filter == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    MongoDocument command;
    command.append("find", collection);
    command.append("filter", filter->document);
    if (projection != nullptr) {
        command.append("projection", projection->document);
    }
    command.append("limit", int32_t(1));
    command.append("singleBatch", true);
    command.set("$db", database);
    *out = make_document(std::move(command));
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_command_insert_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* document,
                                              galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        document == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    MongoArray documents;
    documents.append(document->document);
    MongoDocument command;
    command.append("insert", collection);
    command.append("documents", std::move(documents));
    command.set("$db", database);
    *out = make_document(std::move(command));
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_command_update_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* filter,
                                              const galay_mongo_document_t* update,
                                              galay_bool_t upsert,
                                              galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        filter == nullptr || update == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    MongoDocument item;
    item.append("q", filter->document);
    item.append("u", update->document);
    item.append("upsert", upsert == GALAY_TRUE);
    MongoArray updates;
    updates.append(std::move(item));
    MongoDocument command;
    command.append("update", collection);
    command.append("updates", std::move(updates));
    command.set("$db", database);
    *out = make_document(std::move(command));
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_command_delete_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* filter,
                                              galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        filter == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    MongoDocument item;
    item.append("q", filter->document);
    item.append("limit", int32_t(1));
    MongoArray deletes;
    deletes.append(std::move(item));
    MongoDocument command;
    command.append("delete", collection);
    command.append("deletes", std::move(deletes));
    command.set("$db", database);
    *out = make_document(std::move(command));
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_mongo_client_create(galay_mongo_client_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_mongo_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_client_destroy(galay_mongo_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
        }
    }
    delete client;
}

void galay_mongo_client_close(galay_mongo_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    if (client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
            return;
        }
    }
    client->connected = false;
    client->recv_buffer.clear();
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

galay_status_t galay_mongo_client_set_endpoint(galay_mongo_client_t* client, const char* host, uint16_t port, const char* database)
{
    if (client == nullptr || host == nullptr || host[0] == '\0' || port == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    client->host = host;
    client->port = port;
    client->database = database == nullptr || database[0] == '\0' ? "admin" : database;
    return GALAY_OK;
}

C_IOResult galay_mongo_client_connect_async(galay_mongo_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->connected || client->socket.socket != nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(client->host, client->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) {
        return io_result_from_socket_create(created);
    }

    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket, &host, timeout_ms);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return connected;
    }
    client->connected = true;
    connected.ptr = client;
    return connected;
}

C_IOResult galay_mongo_client_hello_async(galay_mongo_client_t* client, int64_t timeout_ms, galay_mongo_document_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || reply == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    MongoDocument hello;
    hello.append("hello", int32_t(1));
    hello.append("helloOk", true);
    return send_command(client, client->database.c_str(), hello, timeout_ms, reply);
}

C_IOResult galay_mongo_client_command_async(galay_mongo_client_t* client, const char* database,
                                            const galay_mongo_document_t* command,
                                            int64_t timeout_ms,
                                            galay_mongo_document_t** reply)
{
    if (command == nullptr) {
        if (reply != nullptr) *reply = nullptr;
        return make_io_result(C_IOResultInvalid);
    }
    const char* effective_database =
        database == nullptr || database[0] == '\0'
            ? (client == nullptr ? nullptr : client->database.c_str())
            : database;
    return send_command(client, effective_database, command->document, timeout_ms, reply);
}

C_IOResult galay_mongo_client_close_async(galay_mongo_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult close_result = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->connected = false;
    client->recv_buffer.clear();
    if (close_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        return io_result_from_socket_create(destroyed);
    }
    return close_result;
}

}
