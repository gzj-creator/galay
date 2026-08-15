#include <galay/c/galay-mongo-c/mongo.h>

#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

constexpr size_t kMongoMaxMessageSize = 16 * 1024 * 1024;
constexpr uint8_t kBsonDouble = 0x01;
constexpr uint8_t kBsonString = 0x02;
constexpr uint8_t kBsonDocument = 0x03;
constexpr uint8_t kBsonArray = 0x04;
constexpr uint8_t kBsonBinary = 0x05;
constexpr uint8_t kBsonObjectId = 0x07;
constexpr uint8_t kBsonBool = 0x08;
constexpr uint8_t kBsonDateTime = 0x09;
constexpr uint8_t kBsonNull = 0x0a;
constexpr uint8_t kBsonInt32 = 0x10;
constexpr uint8_t kBsonTimestamp = 0x11;
constexpr uint8_t kBsonInt64 = 0x12;

struct galay_mongo_document_t {
    std::vector<uint8_t> bson{5, 0, 0, 0, 0};
    mutable std::unordered_map<std::string, std::string> object_id_cache;
};

struct galay_mongo_array_t {
    std::vector<uint8_t> bson{5, 0, 0, 0, 0};
};

struct galay_mongo_uri_t {
    std::string host;
    std::string database;
    uint16_t port = 27017;
};

struct galay_mongo_client_t {
    std::string host = "127.0.0.1";
    std::string database = "admin";
    std::vector<uint8_t> recv_buffer;
    galay_c_tcp_socket_t socket{};
    int32_t next_request_id = 1;
    uint16_t port = 27017;
    bool connected = false;
};

struct BsonElementView {
    const uint8_t* value = nullptr;
    size_t value_size = 0;
    uint8_t type = 0;
};

C_IOResult make_io_result(C_IOResultCode code, int64_t value = 0)
{
    return C_IOResult{code, 0, 0, value, nullptr};
}

C_IOResult io_result_from_status(galay_status_t status)
{
    return make_io_result(status == GALAY_INVALID_ARGUMENT ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(status));
}

uint32_t read_u32_le(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) |
        (static_cast<uint32_t>(data[2]) << 16U) |
        (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t read_u64_le(const uint8_t* data)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8U);
    }
    return value;
}

void write_u32_le(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xffU);
    out[1] = static_cast<uint8_t>((value >> 8U) & 0xffU);
    out[2] = static_cast<uint8_t>((value >> 16U) & 0xffU);
    out[3] = static_cast<uint8_t>((value >> 24U) & 0xffU);
}

void write_u64_le(uint8_t* out, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
    }
}

void append_u32_le(std::vector<uint8_t>& out, uint32_t value)
{
    const size_t offset = out.size();
    out.resize(offset + 4);
    write_u32_le(out.data() + offset, value);
}

bool bson_valid(const uint8_t* data, size_t size);

bool bson_value_size(uint8_t type, const uint8_t* data, size_t remaining, size_t* out)
{
    if (out == nullptr) {
        return false;
    }
    switch (type) {
        case kBsonDouble:
        case kBsonDateTime:
        case kBsonTimestamp:
        case kBsonInt64:
            *out = 8;
            return remaining >= *out;
        case kBsonInt32:
            *out = 4;
            return remaining >= *out;
        case kBsonObjectId:
            *out = 12;
            return remaining >= *out;
        case kBsonBool:
            *out = 1;
            return remaining >= 1 && (data[0] == 0 || data[0] == 1);
        case kBsonNull:
            *out = 0;
            return true;
        case kBsonString: {
            if (remaining < 4) {
                return false;
            }
            const uint32_t length = read_u32_le(data);
            if (length == 0 || static_cast<size_t>(length) > remaining - 4) {
                return false;
            }
            *out = 4 + static_cast<size_t>(length);
            return data[*out - 1] == 0;
        }
        case kBsonDocument:
        case kBsonArray: {
            if (remaining < 4) {
                return false;
            }
            const uint32_t length = read_u32_le(data);
            if (length < 5 || static_cast<size_t>(length) > remaining) {
                return false;
            }
            *out = length;
            return bson_valid(data, length);
        }
        case kBsonBinary: {
            if (remaining < 5) {
                return false;
            }
            const uint32_t length = read_u32_le(data);
            if (static_cast<size_t>(length) > remaining - 5) {
                return false;
            }
            *out = 5 + static_cast<size_t>(length);
            return true;
        }
        default:
            return false;
    }
}

bool bson_next(const uint8_t* data,
               size_t size,
               size_t* position,
               std::string_view* key,
               BsonElementView* element)
{
    if (data == nullptr || position == nullptr || *position >= size - 1) {
        return false;
    }
    size_t pos = *position;
    const uint8_t type = data[pos++];
    const size_t key_begin = pos;
    while (pos < size - 1 && data[pos] != 0) {
        ++pos;
    }
    if (pos >= size - 1) {
        return false;
    }
    if (key != nullptr) {
        *key = std::string_view(reinterpret_cast<const char*>(data + key_begin), pos - key_begin);
    }
    ++pos;
    size_t value_size = 0;
    if (!bson_value_size(type, data + pos, size - 1 - pos, &value_size)) {
        return false;
    }
    if (element != nullptr) {
        element->type = type;
        element->value = data + pos;
        element->value_size = value_size;
    }
    *position = pos + value_size;
    return true;
}

bool bson_valid(const uint8_t* data, size_t size)
{
    if (data == nullptr || size < 5 || size > kMongoMaxMessageSize ||
        read_u32_le(data) != size || data[size - 1] != 0) {
        return false;
    }
    size_t position = 4;
    while (position < size - 1) {
        if (!bson_next(data, size, &position, nullptr, nullptr)) {
            return false;
        }
    }
    return position == size - 1;
}

size_t bson_element_count(const std::vector<uint8_t>& bson)
{
    size_t position = 4;
    size_t count = 0;
    while (position < bson.size() - 1 &&
           bson_next(bson.data(), bson.size(), &position, nullptr, nullptr)) {
        ++count;
    }
    return count;
}

bool bson_find(const std::vector<uint8_t>& bson, std::string_view wanted, BsonElementView* out)
{
    size_t position = 4;
    while (position < bson.size() - 1) {
        std::string_view key;
        BsonElementView element;
        if (!bson_next(bson.data(), bson.size(), &position, &key, &element)) {
            return false;
        }
        if (key == wanted) {
            if (out != nullptr) {
                *out = element;
            }
            return true;
        }
    }
    return false;
}

bool bson_at(const std::vector<uint8_t>& bson, size_t index, BsonElementView* out)
{
    size_t position = 4;
    size_t current = 0;
    while (position < bson.size() - 1) {
        BsonElementView element;
        if (!bson_next(bson.data(), bson.size(), &position, nullptr, &element)) {
            return false;
        }
        if (current++ == index) {
            if (out != nullptr) {
                *out = element;
            }
            return true;
        }
    }
    return false;
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

galay_status_t bson_append(std::vector<uint8_t>& bson,
                           uint8_t type,
                           std::string_view key,
                           const uint8_t* value,
                           size_t value_size)
{
    if (bson.empty() || key.empty() || key.find('\0') != std::string_view::npos ||
        (value == nullptr && value_size != 0) ||
        bson.size() + 1 + key.size() + 1 + value_size > kMongoMaxMessageSize ||
        bson.size() + 1 + key.size() + 1 + value_size > static_cast<size_t>(INT32_MAX)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t old_end = bson.size() - 1;
    bson.resize(bson.size() + 1 + key.size() + 1 + value_size);
    size_t pos = old_end;
    bson[pos++] = type;
    std::memcpy(bson.data() + pos, key.data(), key.size());
    pos += key.size();
    bson[pos++] = 0;
    if (value_size != 0) {
        std::memcpy(bson.data() + pos, value, value_size);
        pos += value_size;
    }
    bson[pos] = 0;
    write_u32_le(bson.data(), static_cast<uint32_t>(bson.size()));
    return GALAY_OK;
}

galay_status_t document_append(galay_mongo_document_t* document,
                               const char* key,
                               uint8_t type,
                               const uint8_t* value,
                               size_t value_size)
{
    if (document == nullptr || validate_key(key) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t status = bson_append(document->bson, type, key, value, value_size);
    if (status == GALAY_OK) {
        document->object_id_cache.clear();
    }
    return status;
}

galay_status_t array_append(galay_mongo_array_t* array,
                            uint8_t type,
                            const uint8_t* value,
                            size_t value_size)
{
    if (array == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const std::string key = std::to_string(bson_element_count(array->bson));
    return bson_append(array->bson, type, key, value, value_size);
}

bool decode_object_id(const char* text, uint8_t out[12])
{
    if (text == nullptr || std::strlen(text) != 24) {
        return false;
    }
    for (size_t i = 0; i < 12; ++i) {
        const unsigned char high = static_cast<unsigned char>(text[i * 2]);
        const unsigned char low = static_cast<unsigned char>(text[i * 2 + 1]);
        auto hex = [](unsigned char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        const int a = hex(high);
        const int b = hex(low);
        if (a < 0 || b < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((a << 4) | b);
    }
    return true;
}

std::string encode_object_id(const uint8_t data[12])
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(24, '0');
    for (size_t i = 0; i < 12; ++i) {
        out[i * 2] = digits[(data[i] >> 4U) & 0x0fU];
        out[i * 2 + 1] = digits[data[i] & 0x0fU];
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

C_IOResult socket_read_exact(galay_c_tcp_socket_t* socket,
                             uint8_t* data,
                             size_t data_len,
                             int64_t timeout_ms)
{
    if (socket == nullptr || socket->fd < 0 || (data == nullptr && data_len != 0)) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t received = 0;
    while (received < data_len) {
        C_IOResult result = galay_c_tcp_socket_recv(
            socket, reinterpret_cast<char*>(data + received), data_len - received, timeout_ms);
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

C_IOResult socket_write_exact(galay_c_tcp_socket_t* socket,
                              const uint8_t* data,
                              size_t data_len,
                              int64_t timeout_ms)
{
    if (socket == nullptr || socket->fd < 0 || (data == nullptr && data_len != 0)) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_c_tcp_socket_send(
            socket, reinterpret_cast<const char*>(data + sent), data_len - sent, timeout_ms);
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

C_IOResult send_command(galay_mongo_client_t* client,
                        const char* database,
                        const galay_mongo_document_t* command,
                        int64_t timeout_ms,
                        galay_mongo_document_t** reply)
{
    if (reply != nullptr) {
        *reply = nullptr;
    }
    if (client == nullptr || database == nullptr || database[0] == '\0' || command == nullptr ||
        reply == nullptr || !client->connected || client->socket.fd < 0) {
        return make_io_result(C_IOResultInvalid);
    }

    galay_mongo_document_t outgoing;
    outgoing.bson = command->bson;
    BsonElementView database_value;
    if (!bson_find(outgoing.bson, "$db", &database_value)) {
        const galay_status_t appended = galay_mongo_document_append_string(
            &outgoing, "$db", database, std::strlen(database));
        if (appended != GALAY_OK) {
            return io_result_from_status(appended);
        }
    }

    const int32_t request_id = client->next_request_id++;
    if (client->next_request_id <= 0) {
        client->next_request_id = 1;
    }
    const size_t message_size = 21 + outgoing.bson.size();
    if (message_size > kMongoMaxMessageSize || message_size > static_cast<size_t>(INT32_MAX)) {
        return io_result_from_status(GALAY_INVALID_ARGUMENT);
    }
    std::vector<uint8_t> message;
    message.reserve(message_size);
    append_u32_le(message, static_cast<uint32_t>(message_size));
    append_u32_le(message, static_cast<uint32_t>(request_id));
    append_u32_le(message, 0);
    append_u32_le(message, 2013);
    append_u32_le(message, 0);
    message.push_back(0);
    message.insert(message.end(), outgoing.bson.begin(), outgoing.bson.end());

    C_IOResult sent = socket_write_exact(&client->socket, message.data(), message.size(), timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }

    uint8_t header[16];
    C_IOResult received = socket_read_exact(&client->socket, header, sizeof(header), timeout_ms);
    if (received.code != C_IOResultOk) {
        return received;
    }
    const uint32_t response_size = read_u32_le(header);
    if (response_size < 26 || response_size > kMongoMaxMessageSize ||
        static_cast<int32_t>(read_u32_le(header + 8)) != request_id ||
        read_u32_le(header + 12) != 2013) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    client->recv_buffer.resize(response_size - sizeof(header));
    received = socket_read_exact(&client->socket,
                                 client->recv_buffer.data(),
                                 client->recv_buffer.size(),
                                 timeout_ms);
    if (received.code != C_IOResultOk) {
        client->recv_buffer.clear();
        return received;
    }
    if (read_u32_le(client->recv_buffer.data()) != 0 || client->recv_buffer[4] != 0) {
        client->recv_buffer.clear();
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    const uint8_t* response_bson = client->recv_buffer.data() + 5;
    const size_t response_bson_size = client->recv_buffer.size() - 5;
    if (!bson_valid(response_bson, response_bson_size)) {
        client->recv_buffer.clear();
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    auto* document = new (std::nothrow) galay_mongo_document_t();
    if (document == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    document->bson.assign(response_bson, response_bson + response_bson_size);

    bool ok = false;
    BsonElementView ok_value;
    if (bson_find(document->bson, "ok", &ok_value)) {
        if (ok_value.type == kBsonDouble) {
            const uint64_t bits = read_u64_le(ok_value.value);
            double number = 0.0;
            std::memcpy(&number, &bits, sizeof(number));
            ok = number != 0.0;
        } else if (ok_value.type == kBsonInt32) {
            ok = read_u32_le(ok_value.value) != 0;
        } else if (ok_value.type == kBsonInt64) {
            ok = read_u64_le(ok_value.value) != 0;
        }
    }
    if (!ok) {
        delete document;
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    *reply = document;
    received.ptr = document;
    received.bytes = response_size;
    return received;
}

extern "C" {

const char* galay_mongo_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_mongo_document_create(galay_mongo_document_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_mongo_document_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_document_destroy(galay_mongo_document_t* document)
{
    delete document;
}

size_t galay_mongo_document_size(const galay_mongo_document_t* document)
{
    return document == nullptr ? 0 : bson_element_count(document->bson);
}

galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document,
                                                  const char* key,
                                                  int32_t value)
{
    uint8_t encoded[4];
    write_u32_le(encoded, static_cast<uint32_t>(value));
    return document_append(document, key, kBsonInt32, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document,
                                                  const char* key,
                                                  int64_t value)
{
    uint8_t encoded[8];
    write_u64_le(encoded, static_cast<uint64_t>(value));
    return document_append(document, key, kBsonInt64, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document,
                                                   const char* key,
                                                   double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t encoded[8];
    write_u64_le(encoded, bits);
    return document_append(document, key, kBsonDouble, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document,
                                                 const char* key,
                                                 galay_bool_t value)
{
    const uint8_t encoded = value == GALAY_TRUE ? 1 : 0;
    return document_append(document, key, kBsonBool, &encoded, 1);
}

galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document,
                                                   const char* key,
                                                   const char* value,
                                                   size_t value_len)
{
    if (validate_text(value, value_len) != GALAY_OK) return GALAY_INVALID_ARGUMENT;
    std::vector<uint8_t> encoded;
    encoded.resize(4 + value_len + 1);
    write_u32_le(encoded.data(), static_cast<uint32_t>(value_len + 1));
    if (value_len != 0) std::memcpy(encoded.data() + 4, value, value_len);
    encoded.back() = 0;
    return document_append(document, key, kBsonString, encoded.data(), encoded.size());
}

galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document, const char* key)
{
    return document_append(document, key, kBsonNull, nullptr, 0);
}

galay_status_t galay_mongo_document_append_document(galay_mongo_document_t* document,
                                                     const char* key,
                                                     const galay_mongo_document_t* value)
{
    if (value == nullptr) return GALAY_INVALID_ARGUMENT;
    return document_append(document, key, kBsonDocument, value->bson.data(), value->bson.size());
}

galay_status_t galay_mongo_document_append_array(galay_mongo_document_t* document,
                                                  const char* key,
                                                  const galay_mongo_array_t* value)
{
    if (value == nullptr) return GALAY_INVALID_ARGUMENT;
    return document_append(document, key, kBsonArray, value->bson.data(), value->bson.size());
}

galay_status_t galay_mongo_document_append_binary(galay_mongo_document_t* document,
                                                   const char* key,
                                                   const uint8_t* value,
                                                   size_t value_len)
{
    if ((value == nullptr && value_len != 0) || value_len > static_cast<size_t>(INT32_MAX)) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> encoded(5 + value_len);
    write_u32_le(encoded.data(), static_cast<uint32_t>(value_len));
    encoded[4] = 0;
    if (value_len != 0) std::memcpy(encoded.data() + 5, value, value_len);
    return document_append(document, key, kBsonBinary, encoded.data(), encoded.size());
}

galay_status_t galay_mongo_document_append_object_id(galay_mongo_document_t* document,
                                                      const char* key,
                                                      const char* object_id_hex)
{
    uint8_t object_id[12];
    if (!decode_object_id(object_id_hex, object_id)) return GALAY_INVALID_ARGUMENT;
    return document_append(document, key, kBsonObjectId, object_id, sizeof(object_id));
}

galay_status_t galay_mongo_document_append_date_time(galay_mongo_document_t* document,
                                                      const char* key,
                                                      int64_t millis)
{
    uint8_t encoded[8];
    write_u64_le(encoded, static_cast<uint64_t>(millis));
    return document_append(document, key, kBsonDateTime, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_document_append_timestamp(galay_mongo_document_t* document,
                                                      const char* key,
                                                      uint64_t timestamp)
{
    uint8_t encoded[8];
    write_u64_le(encoded, timestamp);
    return document_append(document, key, kBsonTimestamp, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document,
                                            const uint8_t** bson,
                                            size_t* bson_len)
{
    if (document == nullptr || bson == nullptr || bson_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *bson = document->bson.data();
    *bson_len = document->bson.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_decode(const uint8_t* bson,
                                            size_t bson_len,
                                            galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (bson == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    if (!bson_valid(bson, bson_len)) return GALAY_PROTOCOL_ERROR;
    auto* document = new (std::nothrow) galay_mongo_document_t();
    if (document == nullptr) return GALAY_OUT_OF_MEMORY;
    document->bson.assign(bson, bson + bson_len);
    *out = document;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document,
                                               const char* key,
                                               int32_t* value)
{
    if (document == nullptr || key == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonInt32) return GALAY_INVALID_ARGUMENT;
    *value = static_cast<int32_t>(read_u32_le(found.value));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document,
                                               const char* key,
                                               int64_t* value)
{
    if (document == nullptr || key == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonInt64) return GALAY_INVALID_ARGUMENT;
    *value = static_cast<int64_t>(read_u64_le(found.value));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document,
                                                const char* key,
                                                double* value)
{
    if (document == nullptr || key == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonDouble) return GALAY_INVALID_ARGUMENT;
    const uint64_t bits = read_u64_le(found.value);
    std::memcpy(value, &bits, sizeof(*value));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document,
                                              const char* key,
                                              galay_bool_t* value)
{
    if (document == nullptr || key == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonBool) return GALAY_INVALID_ARGUMENT;
    *value = found.value[0] != 0 ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document,
                                                const char* key,
                                                const char** value,
                                                size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    if (document == nullptr || key == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonString) return GALAY_INVALID_ARGUMENT;
    *value = reinterpret_cast<const char*>(found.value + 4);
    *value_len = read_u32_le(found.value) - 1;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_document(const galay_mongo_document_t* document,
                                                  const char* key,
                                                  galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (document == nullptr || key == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonDocument) return GALAY_INVALID_ARGUMENT;
    return galay_mongo_document_decode(found.value, found.value_size, out);
}

galay_status_t galay_mongo_document_get_array(const galay_mongo_document_t* document,
                                               const char* key,
                                               galay_mongo_array_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (document == nullptr || key == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonArray) return GALAY_INVALID_ARGUMENT;
    auto* array = new (std::nothrow) galay_mongo_array_t();
    if (array == nullptr) return GALAY_OUT_OF_MEMORY;
    array->bson.assign(found.value, found.value + found.value_size);
    *out = array;
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_binary(const galay_mongo_document_t* document,
                                                const char* key,
                                                const uint8_t** value,
                                                size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    if (document == nullptr || key == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonBinary) return GALAY_INVALID_ARGUMENT;
    *value = found.value + 5;
    *value_len = read_u32_le(found.value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_object_id(const galay_mongo_document_t* document,
                                                   const char* key,
                                                   const char** value,
                                                   size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    if (document == nullptr || key == nullptr || value == nullptr || value_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonObjectId) return GALAY_INVALID_ARGUMENT;
    auto inserted = document->object_id_cache.insert_or_assign(key, encode_object_id(found.value));
    *value = inserted.first->second.data();
    *value_len = inserted.first->second.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_date_time(const galay_mongo_document_t* document,
                                                   const char* key,
                                                   int64_t* value)
{
    if (document == nullptr || key == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonDateTime) return GALAY_INVALID_ARGUMENT;
    *value = static_cast<int64_t>(read_u64_le(found.value));
    return GALAY_OK;
}

galay_status_t galay_mongo_document_get_timestamp(const galay_mongo_document_t* document,
                                                   const char* key,
                                                   uint64_t* value)
{
    if (document == nullptr || key == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonTimestamp) return GALAY_INVALID_ARGUMENT;
    *value = read_u64_le(found.value);
    return GALAY_OK;
}

galay_status_t galay_mongo_document_is_null(const galay_mongo_document_t* document, const char* key)
{
    if (document == nullptr || key == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_find(document->bson, key, &found)) return GALAY_NOT_FOUND;
    return found.type == kBsonNull ? GALAY_OK : GALAY_INVALID_ARGUMENT;
}

galay_status_t galay_mongo_array_create(galay_mongo_array_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_mongo_array_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mongo_array_destroy(galay_mongo_array_t* array)
{
    delete array;
}

size_t galay_mongo_array_size(const galay_mongo_array_t* array)
{
    return array == nullptr ? 0 : bson_element_count(array->bson);
}

galay_status_t galay_mongo_array_append_int32(galay_mongo_array_t* array, int32_t value)
{
    uint8_t encoded[4];
    write_u32_le(encoded, static_cast<uint32_t>(value));
    return array_append(array, kBsonInt32, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_array_append_int64(galay_mongo_array_t* array, int64_t value)
{
    uint8_t encoded[8];
    write_u64_le(encoded, static_cast<uint64_t>(value));
    return array_append(array, kBsonInt64, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_array_append_double(galay_mongo_array_t* array, double value)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t encoded[8];
    write_u64_le(encoded, bits);
    return array_append(array, kBsonDouble, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_array_append_bool(galay_mongo_array_t* array, galay_bool_t value)
{
    const uint8_t encoded = value == GALAY_TRUE ? 1 : 0;
    return array_append(array, kBsonBool, &encoded, 1);
}

galay_status_t galay_mongo_array_append_string(galay_mongo_array_t* array,
                                                const char* value,
                                                size_t value_len)
{
    if (validate_text(value, value_len) != GALAY_OK) return GALAY_INVALID_ARGUMENT;
    std::vector<uint8_t> encoded(4 + value_len + 1);
    write_u32_le(encoded.data(), static_cast<uint32_t>(value_len + 1));
    if (value_len != 0) std::memcpy(encoded.data() + 4, value, value_len);
    encoded.back() = 0;
    return array_append(array, kBsonString, encoded.data(), encoded.size());
}

galay_status_t galay_mongo_array_append_null(galay_mongo_array_t* array)
{
    return array_append(array, kBsonNull, nullptr, 0);
}

galay_status_t galay_mongo_array_append_document(galay_mongo_array_t* array,
                                                  const galay_mongo_document_t* value)
{
    if (value == nullptr) return GALAY_INVALID_ARGUMENT;
    return array_append(array, kBsonDocument, value->bson.data(), value->bson.size());
}

galay_status_t galay_mongo_array_append_array(galay_mongo_array_t* array,
                                               const galay_mongo_array_t* value)
{
    if (value == nullptr) return GALAY_INVALID_ARGUMENT;
    return array_append(array, kBsonArray, value->bson.data(), value->bson.size());
}

galay_status_t galay_mongo_array_append_binary(galay_mongo_array_t* array,
                                                const uint8_t* value,
                                                size_t value_len)
{
    if ((value == nullptr && value_len != 0) || value_len > static_cast<size_t>(INT32_MAX)) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::vector<uint8_t> encoded(5 + value_len);
    write_u32_le(encoded.data(), static_cast<uint32_t>(value_len));
    encoded[4] = 0;
    if (value_len != 0) std::memcpy(encoded.data() + 5, value, value_len);
    return array_append(array, kBsonBinary, encoded.data(), encoded.size());
}

galay_status_t galay_mongo_array_append_object_id(galay_mongo_array_t* array,
                                                   const char* object_id_hex)
{
    uint8_t object_id[12];
    if (!decode_object_id(object_id_hex, object_id)) return GALAY_INVALID_ARGUMENT;
    return array_append(array, kBsonObjectId, object_id, sizeof(object_id));
}

galay_status_t galay_mongo_array_append_date_time(galay_mongo_array_t* array, int64_t millis)
{
    uint8_t encoded[8];
    write_u64_le(encoded, static_cast<uint64_t>(millis));
    return array_append(array, kBsonDateTime, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_array_append_timestamp(galay_mongo_array_t* array, uint64_t timestamp)
{
    uint8_t encoded[8];
    write_u64_le(encoded, timestamp);
    return array_append(array, kBsonTimestamp, encoded, sizeof(encoded));
}

galay_status_t galay_mongo_array_get_int32(const galay_mongo_array_t* array,
                                            size_t index,
                                            int32_t* value)
{
    if (array == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonInt32) return GALAY_INVALID_ARGUMENT;
    *value = static_cast<int32_t>(read_u32_le(found.value));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_int64(const galay_mongo_array_t* array,
                                            size_t index,
                                            int64_t* value)
{
    if (array == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonInt64) return GALAY_INVALID_ARGUMENT;
    *value = static_cast<int64_t>(read_u64_le(found.value));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_double(const galay_mongo_array_t* array,
                                             size_t index,
                                             double* value)
{
    if (array == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonDouble) return GALAY_INVALID_ARGUMENT;
    const uint64_t bits = read_u64_le(found.value);
    std::memcpy(value, &bits, sizeof(*value));
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_bool(const galay_mongo_array_t* array,
                                           size_t index,
                                           galay_bool_t* value)
{
    if (array == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonBool) return GALAY_INVALID_ARGUMENT;
    *value = found.value[0] != 0 ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_string(const galay_mongo_array_t* array,
                                             size_t index,
                                             const char** value,
                                             size_t* value_len)
{
    if (value != nullptr) *value = nullptr;
    if (value_len != nullptr) *value_len = 0;
    if (array == nullptr || value == nullptr || value_len == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonString) return GALAY_INVALID_ARGUMENT;
    *value = reinterpret_cast<const char*>(found.value + 4);
    *value_len = read_u32_le(found.value) - 1;
    return GALAY_OK;
}

galay_status_t galay_mongo_array_get_document(const galay_mongo_array_t* array,
                                               size_t index,
                                               galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (array == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonDocument) return GALAY_INVALID_ARGUMENT;
    return galay_mongo_document_decode(found.value, found.value_size, out);
}

galay_status_t galay_mongo_array_get_array(const galay_mongo_array_t* array,
                                            size_t index,
                                            galay_mongo_array_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (array == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    BsonElementView found;
    if (!bson_at(array->bson, index, &found)) return GALAY_NOT_FOUND;
    if (found.type != kBsonArray) return GALAY_INVALID_ARGUMENT;
    auto* nested = new (std::nothrow) galay_mongo_array_t();
    if (nested == nullptr) return GALAY_OUT_OF_MEMORY;
    nested->bson.assign(found.value, found.value + found.value_size);
    *out = nested;
    return GALAY_OK;
}

galay_status_t galay_mongo_uri_parse(const char* uri_text, galay_mongo_uri_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (uri_text == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    const std::string text(uri_text);
    const std::string prefix = "mongodb://";
    if (text.rfind(prefix, 0) != 0) return GALAY_INVALID_ARGUMENT;
    const size_t slash = text.find('/', prefix.size());
    if (slash == std::string::npos || slash + 1 >= text.size()) return GALAY_INVALID_ARGUMENT;
    const std::string host_port = text.substr(prefix.size(), slash - prefix.size());
    const size_t query = text.find('?', slash + 1);
    const std::string database = text.substr(
        slash + 1, query == std::string::npos ? std::string::npos : query - slash - 1);
    if (database.empty() || host_port.empty()) return GALAY_INVALID_ARGUMENT;

    std::string host;
    std::string port_text;
    if (host_port.front() == '[') {
        const size_t close = host_port.find(']');
        if (close == std::string::npos || close == 1) return GALAY_INVALID_ARGUMENT;
        host = host_port.substr(1, close - 1);
        if (close + 1 < host_port.size()) {
            if (host_port[close + 1] != ':') return GALAY_INVALID_ARGUMENT;
            port_text = host_port.substr(close + 2);
        }
    } else {
        const size_t colon = host_port.find(':');
        if (colon != std::string::npos && host_port.find(':', colon + 1) != std::string::npos) {
            return GALAY_INVALID_ARGUMENT;
        }
        host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
        if (colon != std::string::npos) port_text = host_port.substr(colon + 1);
    }
    if (host.empty()) return GALAY_INVALID_ARGUMENT;
    uint16_t port = 27017;
    if (!port_text.empty()) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(port_text.c_str(), &end, 10);
        if (end == port_text.c_str() || *end != '\0' || parsed == 0 || parsed > 65535) {
            return GALAY_INVALID_ARGUMENT;
        }
        port = static_cast<uint16_t>(parsed);
    } else if (host_port.back() == ':') {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* uri = new (std::nothrow) galay_mongo_uri_t();
    if (uri == nullptr) return GALAY_OUT_OF_MEMORY;
    uri->host = host;
    uri->database = database;
    uri->port = port;
    *out = uri;
    return GALAY_OK;
}

void galay_mongo_uri_destroy(galay_mongo_uri_t* uri)
{
    delete uri;
}

galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri,
                                     const char** host,
                                     size_t* host_len)
{
    if (uri == nullptr || host == nullptr || host_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *host = uri->host.data();
    *host_len = uri->host.size();
    return GALAY_OK;
}

galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri,
                                         const char** database,
                                         size_t* database_len)
{
    if (uri == nullptr || database == nullptr || database_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
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

galay_status_t galay_mongo_command_find_one(const char* database,
                                             const char* collection,
                                             const galay_mongo_document_t* filter,
                                             const galay_mongo_document_t* projection,
                                             galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        filter == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    galay_mongo_document_t* command = nullptr;
    galay_status_t status = galay_mongo_document_create(&command);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "find", collection, std::strlen(collection));
    if (status == GALAY_OK) status = galay_mongo_document_append_document(command, "filter", filter);
    if (status == GALAY_OK && projection != nullptr) status = galay_mongo_document_append_document(command, "projection", projection);
    if (status == GALAY_OK) status = galay_mongo_document_append_int32(command, "limit", 1);
    if (status == GALAY_OK) status = galay_mongo_document_append_bool(command, "singleBatch", GALAY_TRUE);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "$db", database, std::strlen(database));
    if (status != GALAY_OK) {
        galay_mongo_document_destroy(command);
        return status;
    }
    *out = command;
    return GALAY_OK;
}

galay_status_t galay_mongo_command_insert_one(const char* database,
                                               const char* collection,
                                               const galay_mongo_document_t* document,
                                               galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        document == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    galay_mongo_array_t* documents = nullptr;
    galay_mongo_document_t* command = nullptr;
    galay_status_t status = galay_mongo_array_create(&documents);
    if (status == GALAY_OK) status = galay_mongo_array_append_document(documents, document);
    if (status == GALAY_OK) status = galay_mongo_document_create(&command);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "insert", collection, std::strlen(collection));
    if (status == GALAY_OK) status = galay_mongo_document_append_array(command, "documents", documents);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "$db", database, std::strlen(database));
    galay_mongo_array_destroy(documents);
    if (status != GALAY_OK) {
        galay_mongo_document_destroy(command);
        return status;
    }
    *out = command;
    return GALAY_OK;
}

galay_status_t galay_mongo_command_update_one(const char* database,
                                               const char* collection,
                                               const galay_mongo_document_t* filter,
                                               const galay_mongo_document_t* update,
                                               galay_bool_t upsert,
                                               galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        filter == nullptr || update == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    galay_mongo_document_t* item = nullptr;
    galay_mongo_array_t* updates = nullptr;
    galay_mongo_document_t* command = nullptr;
    galay_status_t status = galay_mongo_document_create(&item);
    if (status == GALAY_OK) status = galay_mongo_document_append_document(item, "q", filter);
    if (status == GALAY_OK) status = galay_mongo_document_append_document(item, "u", update);
    if (status == GALAY_OK) status = galay_mongo_document_append_bool(item, "upsert", upsert);
    if (status == GALAY_OK) status = galay_mongo_array_create(&updates);
    if (status == GALAY_OK) status = galay_mongo_array_append_document(updates, item);
    if (status == GALAY_OK) status = galay_mongo_document_create(&command);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "update", collection, std::strlen(collection));
    if (status == GALAY_OK) status = galay_mongo_document_append_array(command, "updates", updates);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "$db", database, std::strlen(database));
    galay_mongo_array_destroy(updates);
    galay_mongo_document_destroy(item);
    if (status != GALAY_OK) {
        galay_mongo_document_destroy(command);
        return status;
    }
    *out = command;
    return GALAY_OK;
}

galay_status_t galay_mongo_command_delete_one(const char* database,
                                               const char* collection,
                                               const galay_mongo_document_t* filter,
                                               galay_mongo_document_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (database == nullptr || collection == nullptr || collection[0] == '\0' ||
        filter == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    galay_mongo_document_t* item = nullptr;
    galay_mongo_array_t* deletes = nullptr;
    galay_mongo_document_t* command = nullptr;
    galay_status_t status = galay_mongo_document_create(&item);
    if (status == GALAY_OK) status = galay_mongo_document_append_document(item, "q", filter);
    if (status == GALAY_OK) status = galay_mongo_document_append_int32(item, "limit", 1);
    if (status == GALAY_OK) status = galay_mongo_array_create(&deletes);
    if (status == GALAY_OK) status = galay_mongo_array_append_document(deletes, item);
    if (status == GALAY_OK) status = galay_mongo_document_create(&command);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "delete", collection, std::strlen(collection));
    if (status == GALAY_OK) status = galay_mongo_document_append_array(command, "deletes", deletes);
    if (status == GALAY_OK) status = galay_mongo_document_append_string(command, "$db", database, std::strlen(database));
    galay_mongo_array_destroy(deletes);
    galay_mongo_document_destroy(item);
    if (status != GALAY_OK) {
        galay_mongo_document_destroy(command);
        return status;
    }
    *out = command;
    return GALAY_OK;
}

galay_status_t galay_mongo_client_create(galay_mongo_client_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_mongo_client_t();
    if (*out == nullptr) return GALAY_OUT_OF_MEMORY;
    (*out)->socket.fd = -1;
    return GALAY_OK;
}

void galay_mongo_client_destroy(galay_mongo_client_t* client)
{
    if (client != nullptr && client->socket.fd >= 0) {
        const C_IOResult closed = galay_c_tcp_socket_close(&client->socket);
        (void)closed; /* void destroy cannot propagate an OS close failure. */
    }
    delete client;
}

void galay_mongo_client_close(galay_mongo_client_t* client)
{
    if (client == nullptr) return;
    if (client->socket.fd >= 0) {
        const C_IOResult closed = galay_c_tcp_socket_close(&client->socket);
        (void)closed; /* legacy void API cannot propagate an OS close failure. */
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

galay_status_t galay_mongo_client_set_endpoint(galay_mongo_client_t* client,
                                                const char* host,
                                                uint16_t port,
                                                const char* database)
{
    if (client == nullptr || host == nullptr || host[0] == '\0' || port == 0 ||
        client->socket.fd >= 0) return GALAY_INVALID_ARGUMENT;
    client->host = host;
    client->port = port;
    client->database = database == nullptr || database[0] == '\0' ? "admin" : database;
    return GALAY_OK;
}

C_IOResult galay_mongo_client_connect_async(galay_mongo_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->connected || client->socket.fd >= 0 || timeout_ms < -1) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(client->host, client->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult created = galay_c_tcp_socket_create(&client->socket, host.type);
    if (created.code != C_IOResultOk) return created;
    C_IOResult connected = galay_c_tcp_socket_connect(&client->socket, &host, timeout_ms);
    if (connected.code != C_IOResultOk) {
        C_IOResult closed = galay_c_tcp_socket_close(&client->socket);
        client->connected = false;
        return closed.code == C_IOResultOk ? connected : closed;
    }
    client->connected = true;
    connected.ptr = client;
    return connected;
}

C_IOResult galay_mongo_client_hello_async(galay_mongo_client_t* client,
                                           int64_t timeout_ms,
                                           galay_mongo_document_t** reply)
{
    if (reply != nullptr) *reply = nullptr;
    if (client == nullptr || reply == nullptr) return make_io_result(C_IOResultInvalid);
    galay_mongo_document_t hello;
    galay_status_t status = galay_mongo_document_append_int32(&hello, "hello", 1);
    if (status == GALAY_OK) status = galay_mongo_document_append_bool(&hello, "helloOk", GALAY_TRUE);
    if (status != GALAY_OK) return io_result_from_status(status);
    return send_command(client, client->database.c_str(), &hello, timeout_ms, reply);
}

C_IOResult galay_mongo_client_command_async(galay_mongo_client_t* client,
                                             const char* database,
                                             const galay_mongo_document_t* command,
                                             int64_t timeout_ms,
                                             galay_mongo_document_t** reply)
{
    const char* effective_database = database == nullptr || database[0] == '\0'
        ? (client == nullptr ? nullptr : client->database.c_str())
        : database;
    return send_command(client, effective_database, command, timeout_ms, reply);
}

C_IOResult galay_mongo_client_close_async(galay_mongo_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.fd < 0 || timeout_ms < -1) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult closed = galay_c_tcp_socket_close(&client->socket);
    client->connected = false;
    client->recv_buffer.clear();
    return closed;
}

}
