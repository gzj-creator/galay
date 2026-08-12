#include <galay/c/galay-postgres-c/postgres_c.h>
#include <galay/c/galay-kernel-c/async-c/async_tcp_c.h>
#include <galay/c/galay-kernel-c/common-c/host.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait_c.h>

#include <chrono>

#include <galay/cpp/galay-utils/crypto/hmac.hpp>
#include <galay/cpp/galay-utils/crypto/md5.hpp>
#include <galay/cpp/galay-utils/crypto/pbkdf2.hpp>
#include <galay/cpp/galay-utils/crypto/salt.hpp>
#include <galay/cpp/galay-utils/encoding/base64.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::kernel::c_api::detail
{

[[nodiscard]] C_IOResult tcpSocketRecv(galay_kernel_tcp_socket_t* socket,
                                       char* buffer,
                                       size_t length,
                                       int64_t timeout_ms,
                                       C_CoroWaitRequest* wait_request) noexcept;
[[nodiscard]] C_IOResult tcpSocketSend(galay_kernel_tcp_socket_t* socket,
                                       const char* buffer,
                                       size_t length,
                                       int64_t timeout_ms,
                                       C_CoroWaitRequest* wait_request) noexcept;

} // namespace galay::kernel::c_api::detail

namespace
{

constexpr size_t kHeaderSize = 5;
constexpr size_t kLengthSize = 4;
constexpr size_t kMaxStartupLength = 10000;
constexpr size_t kMaxMessageLength = 64U * 1024U * 1024U;
constexpr size_t kReceiveChunkSize = 8U * 1024U;

struct Field {
    std::string name;
    uint32_t table_oid = 0;
    uint32_t type_oid = 0;
    int32_t type_modifier = -1;
    int16_t column_index = 0;
    int16_t type_size = -1;
    int16_t format = 0;
};

struct ResultSet {
    std::vector<Field> fields;
    std::vector<std::vector<std::optional<std::string>>> rows;
    std::string command_tag;
    uint64_t affected_rows = 0;
    size_t field_count = 0;
    size_t row_count = 0;
    char transaction_status = 'I';
};

uint16_t readU16(const unsigned char* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) | data[1]);
}

uint32_t readU32(const unsigned char* data)
{
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) |
           static_cast<uint32_t>(data[3]);
}

int16_t readI16(const unsigned char* data)
{
    const uint16_t value = readU16(data);
    int16_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

int32_t readI32(const unsigned char* data)
{
    const uint32_t value = readU32(data);
    int32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void writeU16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
    out.push_back(static_cast<char>(value & 0xffU));
}

void writeU32(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24U) & 0xffU));
    out.push_back(static_cast<char>((value >> 16U) & 0xffU));
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
    out.push_back(static_cast<char>(value & 0xffU));
}

bool hasNull(std::string_view value)
{
    return value.find('\0') != std::string_view::npos;
}

void writeCString(std::string& out, std::string_view value)
{
    out.append(value.data(), value.size());
    out.push_back('\0');
}

bool readCString(const unsigned char* data,
                 size_t data_len,
                 size_t* position,
                 std::string* value)
{
    if (data == nullptr || position == nullptr || value == nullptr || *position >= data_len) {
        return false;
    }
    size_t end = *position;
    while (end < data_len && data[end] != 0) ++end;
    if (end == data_len) return false;
    value->assign(reinterpret_cast<const char*>(data + *position), end - *position);
    *position = end + 1;
    return true;
}

std::string wrapMessage(char type, std::string_view payload)
{
    const size_t maximum = static_cast<size_t>(std::numeric_limits<int32_t>::max()) - kLengthSize;
    if (payload.size() > maximum) return {};
    std::string out;
    out.reserve(1 + kLengthSize + payload.size());
    out.push_back(type);
    writeU32(out, static_cast<uint32_t>(kLengthSize + payload.size()));
    out.append(payload.data(), payload.size());
    return out;
}

C_IOResult ioResult(C_IOResultCode code, galay_status_t status = GALAY_OK)
{
    return C_IOResult{code, 0, 0, static_cast<int64_t>(status), nullptr};
}

C_IOResult statusResult(galay_status_t status)
{
    if (status == GALAY_OK) return ioResult(C_IOResultOk);
    if (status == GALAY_INVALID_ARGUMENT) return ioResult(C_IOResultInvalid, status);
    if (status == GALAY_TIMEOUT) return ioResult(C_IOResultTimeout, status);
    if (status == GALAY_CANCELLED) return ioResult(C_IOResultCancelled, status);
    if (status == GALAY_EOF) return ioResult(C_IOResultEof, status);
    return ioResult(C_IOResultError, status);
}

galay_status_t makeBuffer(std::string data, galay_postgres_buffer_t** out);
void fieldToView(const Field& source, galay_postgres_field_view_t* field);

} // namespace

struct galay_postgres_config_t {
    std::string host = "127.0.0.1";
    std::string username;
    std::string password;
    std::string database;
    std::string application_name;
    uint32_t connect_timeout_ms = 5000;
    uint16_t port = 5432;
    bool tcp_no_delay = true;
};

struct galay_postgres_buffer_t {
    std::string data;
};

struct galay_postgres_result_set_t {
    ResultSet value;
};

struct galay_postgres_stmt_t {
    std::string name;
    std::vector<uint32_t> parameter_types;
    std::vector<Field> fields;
};

struct PipelineCommand {
    std::string encoded;
    bool ready = false;
};

struct galay_postgres_pipeline_t {
    std::vector<PipelineCommand> commands;
};

struct galay_postgres_pipeline_result_t {
    std::vector<galay_postgres_result_set_t> results;
};

struct galay_postgres_client_t {
    std::string receive_buffer;
    std::string query_buffer;
    C_CoroWaitRequest wait_request{nullptr};
    galay_kernel_tcp_socket_t socket{};
    size_t receive_size = 0;
    size_t receive_offset = 0;
    std::atomic_flag operation_active = ATOMIC_FLAG_INIT;
    bool connected = false;
};

struct galay_postgres_pool_t {
    galay_postgres_config_t config;
    size_t max_connections = 0;
    size_t total_connections = 0;
    std::vector<galay_postgres_client_t*> idle;
};

struct galay_postgres_pool_lease_t {
    galay_postgres_pool_t* pool = nullptr;
    galay_postgres_client_t* client = nullptr;
};

namespace
{

bool beginClientOperation(galay_postgres_client_t* client)
{
    return client != nullptr &&
        !client->operation_active.test_and_set(std::memory_order_acquire);
}

void finishClientOperation(galay_postgres_client_t* client)
{
    client->operation_active.clear(std::memory_order_release);
}

C_IOResult initializeClientWaitState(galay_postgres_client_t* client)
{
    if (client->wait_request.request != nullptr) {
        return ioResult(C_IOResultOk);
    }
    return galay_coro_wait_request_create(&client->wait_request);
}

C_IOResult destroyClientWaitState(galay_postgres_client_t* client)
{
    if (client->wait_request.request == nullptr) {
        return ioResult(C_IOResultOk);
    }
    return galay_coro_wait_request_destroy(&client->wait_request);
}

void clearClientProtocolState(galay_postgres_client_t* client)
{
    client->receive_buffer.clear();
    client->query_buffer.clear();
    client->receive_size = 0;
    client->receive_offset = 0;
    client->connected = false;
}

C_IOResult closeClientStorage(galay_postgres_client_t* client)
{
    bool cleanup_failed = false;
    if (client->socket.socket != nullptr) {
        cleanup_failed =
            galay_kernel_tcp_socket_destroy(&client->socket) != C_TcpSocketSuccess;
    }
    clearClientProtocolState(client);
    C_IOResult wait_destroyed = destroyClientWaitState(client);
    return cleanup_failed || wait_destroyed.code != C_IOResultOk
        ? statusResult(GALAY_IO_ERROR) : ioResult(C_IOResultOk);
}

galay_status_t validateConfig(const galay_postgres_config_t* config)
{
    return config == nullptr || config->host.empty() || config->port == 0 ||
                   config->username.empty() || config->connect_timeout_ms == 0
        ? GALAY_INVALID_ARGUMENT : GALAY_OK;
}

galay_status_t makeBuffer(std::string data, galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (out == nullptr || data.empty()) return GALAY_INVALID_ARGUMENT;
    auto* buffer = new (std::nothrow) galay_postgres_buffer_t();
    if (buffer == nullptr) return GALAY_OUT_OF_MEMORY;
    buffer->data = std::move(data);
    *out = buffer;
    return GALAY_OK;
}

void fieldToView(const Field& source, galay_postgres_field_view_t* field)
{
    field->name = source.name.c_str();
    field->table_oid = source.table_oid;
    field->column_index = source.column_index;
    field->type_oid = source.type_oid;
    field->type_size = source.type_size;
    field->type_modifier = source.type_modifier;
    field->format = source.format;
}

void resetResultSet(ResultSet* result)
{
    result->command_tag.clear();
    result->affected_rows = 0;
    result->field_count = 0;
    result->row_count = 0;
    result->transaction_status = 'I';
}

galay_status_t parseRowDescription(const unsigned char* data,
                                   size_t data_len,
                                   std::vector<Field>* fields,
                                   size_t* reusable_count = nullptr)
{
    if (data_len < 2) return GALAY_PROTOCOL_ERROR;
    const int16_t signed_count = readI16(data);
    if (signed_count < 0) return GALAY_PROTOCOL_ERROR;
    const size_t count = static_cast<size_t>(signed_count);
    if (count > (data_len - 2) / 19U) return GALAY_PROTOCOL_ERROR;
    if (reusable_count != nullptr) {
        *reusable_count = 0;
        if (fields->size() < count) fields->resize(count);
    } else {
        fields->clear();
        fields->resize(count);
    }
    size_t position = 2;
    for (size_t index = 0; index < count; ++index) {
        Field& field = (*fields)[index];
        if (!readCString(data, data_len, &position, &field.name) || data_len - position < 18U) {
            return GALAY_PROTOCOL_ERROR;
        }
        field.table_oid = readU32(data + position); position += 4;
        field.column_index = readI16(data + position); position += 2;
        field.type_oid = readU32(data + position); position += 4;
        field.type_size = readI16(data + position); position += 2;
        field.type_modifier = readI32(data + position); position += 4;
        field.format = readI16(data + position); position += 2;
        if (field.format != 0 && field.format != 1) return GALAY_PROTOCOL_ERROR;
    }
    if (position != data_len) return GALAY_PROTOCOL_ERROR;
    if (reusable_count != nullptr) *reusable_count = count;
    return GALAY_OK;
}

galay_status_t parseDataRow(const unsigned char* data,
                            size_t data_len,
                            std::vector<std::optional<std::string>>* row)
{
    if (data_len < 2) return GALAY_PROTOCOL_ERROR;
    const int16_t signed_count = readI16(data);
    if (signed_count < 0) return GALAY_PROTOCOL_ERROR;
    const size_t count = static_cast<size_t>(signed_count);
    if (count > (data_len - 2) / 4U) return GALAY_PROTOCOL_ERROR;
    row->resize(count);
    size_t position = 2;
    for (size_t index = 0; index < count; ++index) {
        if (data_len - position < 4U) return GALAY_PROTOCOL_ERROR;
        const int32_t length = readI32(data + position);
        position += 4;
        if (length == -1) {
            (*row)[index].reset();
        } else {
            if (length < 0 || static_cast<size_t>(length) > data_len - position) {
                return GALAY_PROTOCOL_ERROR;
            }
            auto& value = (*row)[index];
            if (!value) value.emplace();
            value->assign(reinterpret_cast<const char*>(data + position),
                          static_cast<size_t>(length));
            position += static_cast<size_t>(length);
        }
    }
    return position == data_len ? GALAY_OK : GALAY_PROTOCOL_ERROR;
}

galay_status_t parseCommandComplete(const unsigned char* data,
                                    size_t data_len,
                                    ResultSet* result)
{
    size_t position = 0;
    if (!readCString(data, data_len, &position, &result->command_tag) ||
        position != data_len || result->command_tag.empty()) return GALAY_PROTOCOL_ERROR;
    const size_t separator = result->command_tag.rfind(' ');
    const std::string_view numeric = separator == std::string::npos
        ? std::string_view(result->command_tag)
        : std::string_view(result->command_tag).substr(separator + 1);
    uint64_t count = 0;
    const auto parsed = std::from_chars(numeric.data(), numeric.data() + numeric.size(), count);
    if (parsed.ec == std::errc{} && parsed.ptr == numeric.data() + numeric.size()) {
        result->affected_rows = count;
    }
    return GALAY_OK;
}

bool validTransactionStatus(unsigned char status)
{
    return status == 'I' || status == 'T' || status == 'E';
}

struct ResultParseState {
    bool ready = false;
    bool server_error = false;
    bool has_row_description = false;
};

galay_status_t parseResultMessage(const galay_postgres_message_view_t& message,
                                  ResultSet* result,
                                  ResultParseState* state)
{
    if (result == nullptr || state == nullptr || state->ready) {
        return GALAY_PROTOCOL_ERROR;
    }
    switch (message.type) {
    case 'T': {
        const galay_status_t status =
            parseRowDescription(message.payload, message.payload_len,
                                &result->fields, &result->field_count);
        if (status != GALAY_OK) return status;
        state->has_row_description = true;
        return GALAY_OK;
    }
    case 'D': {
        if (!state->has_row_description) return GALAY_PROTOCOL_ERROR;
        if (result->row_count == result->rows.size()) {
            result->rows.emplace_back();
        }
        auto& row = result->rows[result->row_count];
        const galay_status_t status = parseDataRow(message.payload, message.payload_len, &row);
        if (status != GALAY_OK || row.size() != result->field_count) {
            return GALAY_PROTOCOL_ERROR;
        }
        ++result->row_count;
        return GALAY_OK;
    }
    case 'C':
        return parseCommandComplete(message.payload, message.payload_len, result);
    case 'Z':
        if (message.payload_len != 1 || !validTransactionStatus(message.payload[0])) {
            return GALAY_PROTOCOL_ERROR;
        }
        result->transaction_status = static_cast<char>(message.payload[0]);
        state->ready = true;
        return GALAY_OK;
    case 'E':
        state->server_error = true;
        return GALAY_OK;
    case 'I':
    case 'N':
        return GALAY_OK;
    case '1':
    case '2':
    case '3':
    case 'n':
    case 's':
        return message.payload_len == 0 ? GALAY_OK : GALAY_PROTOCOL_ERROR;
    case 't':
        return GALAY_OK;
    default:
        return GALAY_PROTOCOL_ERROR;
    }
}

galay_status_t parseResult(const unsigned char* data, size_t data_len, ResultSet* result)
{
    size_t offset = 0;
    ResultParseState state;
    while (offset < data_len) {
        galay_postgres_message_view_t message{};
        const galay_status_t extracted =
            galay_postgres_extract_message(data + offset, data_len - offset, &message);
        if (extracted != GALAY_OK) return extracted;
        offset += message.consumed;
        const galay_status_t status = parseResultMessage(message, result, &state);
        if (status != GALAY_OK) return status;
        if (state.ready && offset != data_len) return GALAY_PROTOCOL_ERROR;
    }
    return state.ready && !state.server_error ? GALAY_OK : GALAY_PROTOCOL_ERROR;
}

bool encodeQueryInto(std::string_view sql, std::string* out)
{
    constexpr size_t maximum_payload =
        static_cast<size_t>(std::numeric_limits<int32_t>::max()) - kLengthSize;
    if (out == nullptr || hasNull(sql) || sql.size() >= maximum_payload) return false;
    out->clear();
    out->reserve(1U + kLengthSize + sql.size() + 1U);
    out->push_back('Q');
    writeU32(*out, static_cast<uint32_t>(kLengthSize + sql.size() + 1U));
    out->append(sql.data(), sql.size());
    out->push_back('\0');
    return true;
}

std::string encodeQuery(std::string_view sql)
{
    std::string out;
    return encodeQueryInto(sql, &out) ? out : std::string{};
}

std::string encodeParse(std::string_view statement,
                        std::string_view sql,
                        const uint32_t* oids,
                        size_t oid_count)
{
    if (hasNull(statement) || hasNull(sql) ||
        oid_count > static_cast<size_t>(std::numeric_limits<int16_t>::max())) return {};
    std::string payload;
    writeCString(payload, statement);
    writeCString(payload, sql);
    writeU16(payload, static_cast<uint16_t>(oid_count));
    for (size_t index = 0; index < oid_count; ++index) writeU32(payload, oids[index]);
    return wrapMessage('P', payload);
}

std::string encodeNamedMessage(char type, char discriminator, std::string_view name)
{
    if (hasNull(name)) return {};
    std::string payload;
    payload.push_back(discriminator);
    writeCString(payload, name);
    return wrapMessage(type, payload);
}

bool copyHost(const galay_postgres_config_t& config, C_Host* host)
{
    if (host == nullptr || config.host.empty() ||
        config.host.size() >= sizeof(host->address) || config.port == 0) return false;
    host->type = config.host.find(':') == std::string::npos ? C_IPTypeIPV4 : C_IPTypeIPV6;
    std::memset(host->address, 0, sizeof(host->address));
    std::memcpy(host->address, config.host.data(), config.host.size());
    host->port = config.port;
    return true;
}

C_IOResult socketWriteAll(galay_postgres_client_t* client,
                          std::string_view data,
                          int64_t timeout_ms)
{
    size_t sent = 0;
    while (sent < data.size()) {
        C_IOResult result = galay::kernel::c_api::detail::tcpSocketSend(
            &client->socket, data.data() + sent, data.size() - sent,
            timeout_ms, &client->wait_request);
        if (result.code != C_IOResultOk) return result;
        if (result.bytes == 0) return ioResult(C_IOResultEof, GALAY_EOF);
        sent += result.bytes;
    }
    C_IOResult result = ioResult(C_IOResultOk);
    result.bytes = sent;
    return result;
}

C_IOResult socketReadMessage(galay_postgres_client_t* client,
                             std::string_view* message,
                             int64_t timeout_ms)
{
    if (client == nullptr || message == nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    *message = {};

    for (;;) {
        const size_t available = client->receive_size - client->receive_offset;
        if (available >= kHeaderSize) {
            const auto* data = reinterpret_cast<const unsigned char*>(
                client->receive_buffer.data() + client->receive_offset);
            galay_postgres_message_header_t header{};
            const galay_status_t parsed =
                galay_postgres_parse_message_header(data, available, &header);
            if (parsed != GALAY_OK) return statusResult(parsed);
            const size_t total = 1U + static_cast<size_t>(header.length);
            if (total > kMaxMessageLength) return statusResult(GALAY_PROTOCOL_ERROR);
            if (available >= total) {
                *message = std::string_view(
                    client->receive_buffer.data() + client->receive_offset, total);
                client->receive_offset += total;
                C_IOResult result = ioResult(C_IOResultOk);
                result.bytes = total;
                return result;
            }
        }

        if (client->receive_offset != 0) {
            const size_t remaining = client->receive_size - client->receive_offset;
            std::memmove(client->receive_buffer.data(),
                         client->receive_buffer.data() + client->receive_offset,
                         remaining);
            client->receive_size = remaining;
            client->receive_offset = 0;
        }
        if (client->receive_size == client->receive_buffer.size()) {
            if (client->receive_buffer.size() >= kMaxMessageLength) {
                return statusResult(GALAY_PROTOCOL_ERROR);
            }
            const size_t current_capacity = client->receive_buffer.size();
            const size_t growth = std::max(current_capacity, kReceiveChunkSize);
            const size_t next_capacity = current_capacity > kMaxMessageLength - growth
                ? kMaxMessageLength : current_capacity + growth;
            client->receive_buffer.resize(next_capacity);
        }
        if (client->receive_size >= kMaxMessageLength) {
            return statusResult(GALAY_PROTOCOL_ERROR);
        }
        const size_t read_size = client->receive_buffer.size() - client->receive_size;
        C_IOResult result = galay::kernel::c_api::detail::tcpSocketRecv(
            &client->socket,
            client->receive_buffer.data() + client->receive_size,
            read_size,
            timeout_ms,
            &client->wait_request);
        if (result.code != C_IOResultOk) return result;
        if (result.bytes == 0) return ioResult(C_IOResultEof, GALAY_EOF);
        client->receive_size += result.bytes;
    }
}

struct ScramState {
    std::string client_first_bare;
    std::string server_first;
    std::string server_nonce;
    std::vector<uint8_t> salt;
    std::array<uint8_t, 32> expected_server_signature{};
    uint32_t iterations = 0;
};

bool validNonce(std::string_view nonce)
{
    if (nonce.empty()) return false;
    for (unsigned char value : nonce) {
        if (value < 0x21 || value > 0x7e || value == ',') return false;
    }
    return true;
}

bool strictBase64Decode(std::string_view encoded, std::vector<uint8_t>* decoded)
{
    if (encoded.empty() || encoded.size() % 4U != 0 ||
        !galay::utils::Base64Util::Base64CanDecodeView(encoded)) return false;
    const std::string value = galay::utils::Base64Util::Base64DecodeView(encoded);
    if (value.empty()) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
    if (galay::utils::Base64Util::Base64Encode(bytes, value.size()) != encoded) return false;
    decoded->assign(bytes, bytes + value.size());
    return true;
}

bool parseServerFirst(std::string_view server_first,
                      std::string_view client_nonce,
                      ScramState* state)
{
    const size_t first = server_first.find(',');
    const size_t second = first == std::string_view::npos
        ? std::string_view::npos : server_first.find(',', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        server_first.find(',', second + 1) != std::string_view::npos) return false;
    const std::string_view nonce = server_first.substr(0, first);
    const std::string_view salt = server_first.substr(first + 1, second - first - 1);
    const std::string_view iterations = server_first.substr(second + 1);
    if (!nonce.starts_with("r=") || !salt.starts_with("s=") ||
        !iterations.starts_with("i=")) return false;
    const std::string_view server_nonce = nonce.substr(2);
    if (!validNonce(server_nonce) || !server_nonce.starts_with(client_nonce) ||
        server_nonce.size() <= client_nonce.size() ||
        !strictBase64Decode(salt.substr(2), &state->salt)) return false;
    uint32_t count = 0;
    const std::string_view text = iterations.substr(2);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || count == 0) return false;
    state->server_first.assign(server_first);
    state->server_nonce.assign(server_nonce);
    state->iterations = count;
    return true;
}

bool makeClientFinal(std::string_view password,
                     ScramState* state,
                     std::string* client_final)
{
    const auto* password_bytes = reinterpret_cast<const uint8_t*>(password.data());
    std::vector<uint8_t> salted = galay::utils::PBKDF2::hmacSha256(
        password_bytes, password.size(), state->salt.data(), state->salt.size(),
        state->iterations, 32);
    if (salted.size() != 32) return false;
    const std::string without_proof = "c=biws,r=" + state->server_nonce;
    const std::string auth_message = state->client_first_bare + "," +
                                     state->server_first + "," + without_proof;
    static constexpr std::string_view kClientKey = "Client Key";
    static constexpr std::string_view kServerKey = "Server Key";
    const auto client_key = galay::utils::HMAC::hmacSha256(
        salted.data(), salted.size(), reinterpret_cast<const uint8_t*>(kClientKey.data()),
        kClientKey.size());
    const auto stored_key = galay::utils::SHA256::hash(client_key.data(), client_key.size());
    const auto signature = galay::utils::HMAC::hmacSha256(
        stored_key.data(), stored_key.size(),
        reinterpret_cast<const uint8_t*>(auth_message.data()), auth_message.size());
    std::array<uint8_t, 32> proof{};
    for (size_t index = 0; index < proof.size(); ++index) {
        proof[index] = static_cast<uint8_t>(client_key[index] ^ signature[index]);
    }
    const auto server_key = galay::utils::HMAC::hmacSha256(
        salted.data(), salted.size(), reinterpret_cast<const uint8_t*>(kServerKey.data()),
        kServerKey.size());
    state->expected_server_signature = galay::utils::HMAC::hmacSha256(
        server_key.data(), server_key.size(),
        reinterpret_cast<const uint8_t*>(auth_message.data()), auth_message.size());
    std::fill(salted.begin(), salted.end(), uint8_t{0});
    *client_final = without_proof + ",p=" +
        galay::utils::Base64Util::Base64Encode(proof.data(), proof.size());
    return true;
}

bool verifyServerFinal(std::string_view server_final, const ScramState& state)
{
    if (!server_final.starts_with("v=") || server_final.find(',') != std::string_view::npos) {
        return false;
    }
    std::vector<uint8_t> verifier;
    if (!strictBase64Decode(server_final.substr(2), &verifier) ||
        verifier.size() != state.expected_server_signature.size()) return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < verifier.size(); ++index) {
        difference |= static_cast<uint8_t>(verifier[index] ^
                                           state.expected_server_signature[index]);
    }
    return difference == 0;
}

std::string md5Password(std::string_view username,
                        std::string_view password,
                        const unsigned char* salt)
{
    std::string first;
    first.reserve(password.size() + username.size());
    first.append(password);
    first.append(username);
    std::string second = galay::utils::MD5Util::MD5View(first);
    second.append(reinterpret_cast<const char*>(salt), 4);
    return "md5" + galay::utils::MD5Util::MD5View(second);
}

galay_status_t parseAuthKind(const galay_postgres_message_view_t& message, uint32_t* kind)
{
    if (message.type != 'R' || message.payload_len < 4) return GALAY_PROTOCOL_ERROR;
    *kind = readU32(message.payload);
    return GALAY_OK;
}

C_IOResult connectAndAuthenticate(galay_postgres_client_t* client,
                                 const galay_postgres_config_t* config,
                                 int64_t timeout_ms)
{
    C_Host host{};
    if (!copyHost(*config, &host)) return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    const C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) return ioResult(C_IOResultError, GALAY_IO_ERROR);
    if (config->tcp_no_delay) {
        const C_TcpSocketResultCode enabled =
            galay_kernel_tcp_socket_enable_tcp_no_delay(&client->socket);
        if (enabled != C_TcpSocketSuccess) {
            const C_TcpSocketResultCode destroyed =
                galay_kernel_tcp_socket_destroy(&client->socket);
            return destroyed == C_TcpSocketSuccess ? statusResult(GALAY_IO_ERROR)
                                                   : statusResult(GALAY_INTERNAL_ERROR);
        }
    }
    const int64_t effective_timeout = timeout_ms < 0
        ? static_cast<int64_t>(config->connect_timeout_ms) : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket, &host, effective_timeout);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        return destroyed == C_TcpSocketSuccess ? connected : statusResult(GALAY_IO_ERROR);
    }
    galay_postgres_buffer_t* startup = nullptr;
    const galay_status_t encoded = galay_postgres_encode_startup_message(config, &startup);
    if (encoded != GALAY_OK) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        return destroyed == C_TcpSocketSuccess ? statusResult(encoded)
                                               : statusResult(GALAY_IO_ERROR);
    }
    C_IOResult sent = socketWriteAll(client, startup->data, effective_timeout);
    galay_postgres_buffer_destroy(startup);
    if (sent.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        return destroyed == C_TcpSocketSuccess ? sent : statusResult(GALAY_IO_ERROR);
    }

    ScramState scram;
    std::string client_nonce;
    bool auth_ok = false;
    bool scram_started = false;
    bool scram_verified = false;
    for (;;) {
        std::string_view encoded_message;
        C_IOResult read = socketReadMessage(client, &encoded_message, effective_timeout);
        if (read.code != C_IOResultOk) return read;
        galay_postgres_message_view_t message{};
        const galay_status_t extracted = galay_postgres_extract_message(
            reinterpret_cast<const unsigned char*>(encoded_message.data()),
            encoded_message.size(), &message);
        if (extracted != GALAY_OK) return statusResult(extracted);
        if (message.type == 'R') {
            uint32_t kind = 0;
            if (parseAuthKind(message, &kind) != GALAY_OK) return statusResult(GALAY_PROTOCOL_ERROR);
            if (kind == 0) {
                auth_ok = true;
            } else if (kind == 10) {
                bool has_scram = false;
                bool terminated = false;
                size_t position = 4;
                while (position < message.payload_len) {
                    std::string mechanism;
                    if (!readCString(message.payload, message.payload_len, &position, &mechanism)) {
                        return statusResult(GALAY_PROTOCOL_ERROR);
                    }
                    if (mechanism.empty()) {
                        terminated = position == message.payload_len;
                        break;
                    }
                    if (mechanism == "SCRAM-SHA-256") has_scram = true;
                }
                if (!terminated) return statusResult(GALAY_PROTOCOL_ERROR);
                if (!has_scram) return statusResult(GALAY_UNSUPPORTED);
                const std::vector<uint8_t> bytes = galay::utils::SaltGenerator::generateSecureBytes(18);
                if (bytes.size() != 18) return statusResult(GALAY_INTERNAL_ERROR);
                client_nonce = galay::utils::Base64Util::Base64Encode(bytes.data(), bytes.size());
                if (!validNonce(client_nonce)) return statusResult(GALAY_INTERNAL_ERROR);
                scram.client_first_bare = "n=,r=" + client_nonce;
                const std::string client_first = "n,," + scram.client_first_bare;
                galay_postgres_buffer_t* response = nullptr;
                const galay_status_t response_status = galay_postgres_encode_sasl_initial_response(
                    "SCRAM-SHA-256", client_first.c_str(), &response);
                if (response_status != GALAY_OK) return statusResult(response_status);
                sent = socketWriteAll(client, response->data, effective_timeout);
                galay_postgres_buffer_destroy(response);
                if (sent.code != C_IOResultOk) return sent;
                scram_started = true;
            } else if (kind == 11) {
                if (!scram_started || !parseServerFirst(
                        std::string_view(reinterpret_cast<const char*>(message.payload + 4),
                                         message.payload_len - 4),
                        client_nonce, &scram)) return statusResult(GALAY_PROTOCOL_ERROR);
                std::string client_final;
                if (!makeClientFinal(config->password, &scram, &client_final)) {
                    return statusResult(GALAY_INTERNAL_ERROR);
                }
                galay_postgres_buffer_t* response = nullptr;
                const galay_status_t response_status =
                    galay_postgres_encode_sasl_response(client_final.c_str(), &response);
                if (response_status != GALAY_OK) return statusResult(response_status);
                sent = socketWriteAll(client, response->data, effective_timeout);
                galay_postgres_buffer_destroy(response);
                if (sent.code != C_IOResultOk) return sent;
            } else if (kind == 12) {
                const std::string_view server_final(
                    reinterpret_cast<const char*>(message.payload + 4), message.payload_len - 4);
                if (!verifyServerFinal(server_final, scram)) return statusResult(GALAY_PROTOCOL_ERROR);
                scram_verified = true;
            } else if (kind == 5) {
                if (message.payload_len != 8) return statusResult(GALAY_PROTOCOL_ERROR);
                const std::string password = md5Password(config->username, config->password,
                                                         message.payload + 4);
                galay_postgres_buffer_t* response = nullptr;
                const galay_status_t response_status =
                    galay_postgres_encode_password_message(password.c_str(), &response);
                if (response_status != GALAY_OK) return statusResult(response_status);
                sent = socketWriteAll(client, response->data, effective_timeout);
                galay_postgres_buffer_destroy(response);
                if (sent.code != C_IOResultOk) return sent;
            } else if (kind == 3) {
                galay_postgres_buffer_t* response = nullptr;
                const galay_status_t response_status =
                    galay_postgres_encode_password_message(config->password.c_str(), &response);
                if (response_status != GALAY_OK) return statusResult(response_status);
                sent = socketWriteAll(client, response->data, effective_timeout);
                galay_postgres_buffer_destroy(response);
                if (sent.code != C_IOResultOk) return sent;
            } else {
                return statusResult(GALAY_UNSUPPORTED);
            }
        } else if (message.type == 'E') {
            return statusResult(GALAY_PROTOCOL_ERROR);
        } else if (message.type == 'Z') {
            if (message.payload_len != 1 || !validTransactionStatus(message.payload[0]) ||
                !auth_ok || (scram_started && !scram_verified)) {
                return statusResult(GALAY_PROTOCOL_ERROR);
            }
            client->connected = true;
            read.ptr = client;
            return read;
        } else if (message.type != 'S' && message.type != 'K' && message.type != 'N') {
            return statusResult(GALAY_PROTOCOL_ERROR);
        }
    }
}

C_IOResult queryResultInto(galay_postgres_client_t* client,
                           const char* sql,
                           int64_t timeout_ms,
    galay_postgres_result_set_t* result)
{
    resetResultSet(&result->value);
    if (!encodeQueryInto(sql, &client->query_buffer)) {
        return statusResult(GALAY_INVALID_ARGUMENT);
    }
    C_IOResult sent = socketWriteAll(client, client->query_buffer, timeout_ms);
    if (sent.code != C_IOResultOk) return sent;

    ResultParseState state;
    size_t response_bytes = 0;
    while (!state.ready) {
        std::string_view encoded_message;
        C_IOResult read = socketReadMessage(client, &encoded_message, timeout_ms);
        if (read.code != C_IOResultOk) {
            resetResultSet(&result->value);
            return read;
        }
        response_bytes += read.bytes;
        galay_postgres_message_view_t message{};
        galay_status_t status = galay_postgres_extract_message(
            reinterpret_cast<const unsigned char*>(encoded_message.data()),
            encoded_message.size(), &message);
        if (status == GALAY_OK) status = parseResultMessage(message, &result->value, &state);
        if (status != GALAY_OK) {
            resetResultSet(&result->value);
            return statusResult(status);
        }
    }
    if (state.server_error) {
        resetResultSet(&result->value);
        return statusResult(GALAY_PROTOCOL_ERROR);
    }
    C_IOResult io = ioResult(C_IOResultOk);
    io.bytes = response_bytes;
    io.ptr = result;
    return io;
}

C_IOResult queryResult(galay_postgres_client_t* client,
                      const char* sql,
                      int64_t timeout_ms,
                      galay_postgres_result_set_t** result)
{
    auto* decoded = new (std::nothrow) galay_postgres_result_set_t();
    if (decoded == nullptr) return statusResult(GALAY_OUT_OF_MEMORY);
    C_IOResult io = queryResultInto(client, sql, timeout_ms, decoded);
    if (io.code != C_IOResultOk) {
        delete decoded;
        return io;
    }
    *result = decoded;
    return io;
}

galay_status_t parseParameterDescription(const unsigned char* data,
                                         size_t data_len,
                                         std::vector<uint32_t>* oids)
{
    if (data_len < 2) return GALAY_PROTOCOL_ERROR;
    const int16_t signed_count = readI16(data);
    if (signed_count < 0) return GALAY_PROTOCOL_ERROR;
    const size_t count = static_cast<size_t>(signed_count);
    if (data_len != 2U + count * 4U) return GALAY_PROTOCOL_ERROR;
    oids->reserve(count);
    for (size_t index = 0; index < count; ++index) {
        oids->push_back(readU32(data + 2U + index * 4U));
    }
    return GALAY_OK;
}

C_IOResult readPreparedMetadata(galay_postgres_client_t* client,
                                int64_t timeout_ms,
                                galay_postgres_stmt_t* stmt)
{
    bool parse_complete = false;
    bool ready = false;
    bool server_error = false;
    size_t bytes = 0;
    while (!ready) {
        std::string_view encoded_message;
        C_IOResult read = socketReadMessage(client, &encoded_message, timeout_ms);
        if (read.code != C_IOResultOk) return read;
        bytes += read.bytes;
        galay_postgres_message_view_t message{};
        const galay_status_t extracted = galay_postgres_extract_message(
            reinterpret_cast<const unsigned char*>(encoded_message.data()),
            encoded_message.size(), &message);
        if (extracted != GALAY_OK) return statusResult(extracted);
        switch (message.type) {
        case '1':
            if (message.payload_len != 0) return statusResult(GALAY_PROTOCOL_ERROR);
            parse_complete = true;
            break;
        case 't': {
            const galay_status_t status = parseParameterDescription(
                message.payload, message.payload_len, &stmt->parameter_types);
            if (status != GALAY_OK) return statusResult(status);
            break;
        }
        case 'T': {
            const galay_status_t status =
                parseRowDescription(message.payload, message.payload_len, &stmt->fields);
            if (status != GALAY_OK) return statusResult(status);
            break;
        }
        case 'n':
            if (message.payload_len != 0) return statusResult(GALAY_PROTOCOL_ERROR);
            break;
        case 'E':
            server_error = true;
            break;
        case 'N':
            break;
        case 'Z':
            if (message.payload_len != 1 || !validTransactionStatus(message.payload[0])) {
                return statusResult(GALAY_PROTOCOL_ERROR);
            }
            ready = true;
            break;
        default:
            return statusResult(GALAY_PROTOCOL_ERROR);
        }
    }
    if (server_error || !parse_complete) return statusResult(GALAY_PROTOCOL_ERROR);
    C_IOResult result = ioResult(C_IOResultOk);
    result.bytes = bytes;
    result.ptr = stmt;
    return result;
}

C_IOResult readPipelineResults(galay_postgres_client_t* client,
                              size_t expected_ready,
                              int64_t timeout_ms,
                              galay_postgres_pipeline_result_t* pipeline_result)
{
    std::string response;
    size_t total_bytes = 0;
    galay_status_t first_error = GALAY_OK;
    for (size_t ready_count = 0; ready_count < expected_ready;) {
        std::string_view encoded_message;
        C_IOResult read = socketReadMessage(client, &encoded_message, timeout_ms);
        if (read.code != C_IOResultOk) return read;
        total_bytes += read.bytes;
        galay_postgres_message_view_t message{};
        const galay_status_t extracted = galay_postgres_extract_message(
            reinterpret_cast<const unsigned char*>(encoded_message.data()),
            encoded_message.size(), &message);
        if (extracted != GALAY_OK) return statusResult(extracted);
        response.append(encoded_message);
        if (message.type != 'Z') continue;

        galay_postgres_result_set_t decoded;
        const galay_status_t status = parseResult(
            reinterpret_cast<const unsigned char*>(response.data()), response.size(), &decoded.value);
        if (status != GALAY_OK) {
            if (first_error == GALAY_OK) first_error = status;
        } else if (first_error == GALAY_OK) {
            pipeline_result->results.push_back(std::move(decoded));
        }
        response.clear();
        ++ready_count;
    }
    if (first_error != GALAY_OK) return statusResult(first_error);
    C_IOResult result = ioResult(C_IOResultOk);
    result.bytes = total_bytes;
    result.value = static_cast<int64_t>(pipeline_result->results.size());
    result.ptr = pipeline_result;
    return result;
}

} // namespace

extern "C" {

const char* galay_postgres_get_error(galay_status_t status)
{
    switch (status) {
    case GALAY_OK: return "ok";
    case GALAY_INVALID_ARGUMENT: return "invalid argument";
    case GALAY_NOT_FOUND: return "not found";
    case GALAY_OUT_OF_MEMORY: return "out of memory";
    case GALAY_PROTOCOL_ERROR: return "protocol error";
    case GALAY_UNSUPPORTED: return "unsupported";
    case GALAY_IO_ERROR: return "io error";
    case GALAY_INTERNAL_ERROR: return "internal error";
    case GALAY_EOF: return "eof";
    case GALAY_TIMEOUT: return "timeout";
    case GALAY_CANCELLED: return "cancelled";
    }
    return "unknown";
}

galay_status_t galay_postgres_config_create(galay_postgres_config_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_postgres_config_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_postgres_config_destroy(galay_postgres_config_t* config) { delete config; }

#define GALAY_PG_GET_STRING(name, member) \
    galay_status_t name(const galay_postgres_config_t* config, const char** value) \
    { \
        if (config == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT; \
        *value = config->member.c_str(); \
        return GALAY_OK; \
    }

GALAY_PG_GET_STRING(galay_postgres_config_host, host)
GALAY_PG_GET_STRING(galay_postgres_config_username, username)
GALAY_PG_GET_STRING(galay_postgres_config_password, password)
GALAY_PG_GET_STRING(galay_postgres_config_database, database)
GALAY_PG_GET_STRING(galay_postgres_config_application_name, application_name)
#undef GALAY_PG_GET_STRING

galay_status_t galay_postgres_config_port(const galay_postgres_config_t* config, uint16_t* port)
{
    if (config == nullptr || port == nullptr) return GALAY_INVALID_ARGUMENT;
    *port = config->port;
    return GALAY_OK;
}

galay_status_t galay_postgres_config_connect_timeout_ms(const galay_postgres_config_t* config,
                                                        uint32_t* timeout_ms)
{
    if (config == nullptr || timeout_ms == nullptr) return GALAY_INVALID_ARGUMENT;
    *timeout_ms = config->connect_timeout_ms;
    return GALAY_OK;
}

galay_status_t galay_postgres_config_tcp_no_delay(const galay_postgres_config_t* config,
                                                  galay_bool_t* enabled)
{
    if (config == nullptr || enabled == nullptr) return GALAY_INVALID_ARGUMENT;
    *enabled = config->tcp_no_delay ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

#define GALAY_PG_SET_STRING(name, member, allow_empty) \
    galay_status_t name(galay_postgres_config_t* config, const char* value) \
    { \
        if (config == nullptr || value == nullptr || (!(allow_empty) && value[0] == '\0')) \
            return GALAY_INVALID_ARGUMENT; \
        config->member = value; \
        return GALAY_OK; \
    }

GALAY_PG_SET_STRING(galay_postgres_config_set_host, host, false)
GALAY_PG_SET_STRING(galay_postgres_config_set_username, username, false)
GALAY_PG_SET_STRING(galay_postgres_config_set_password, password, true)
GALAY_PG_SET_STRING(galay_postgres_config_set_database, database, true)
GALAY_PG_SET_STRING(galay_postgres_config_set_application_name, application_name, true)
#undef GALAY_PG_SET_STRING

galay_status_t galay_postgres_config_set_port(galay_postgres_config_t* config, uint16_t port)
{
    if (config == nullptr || port == 0) return GALAY_INVALID_ARGUMENT;
    config->port = port;
    return GALAY_OK;
}

galay_status_t galay_postgres_config_set_connect_timeout_ms(galay_postgres_config_t* config,
                                                            uint32_t timeout_ms)
{
    if (config == nullptr || timeout_ms == 0) return GALAY_INVALID_ARGUMENT;
    config->connect_timeout_ms = timeout_ms;
    return GALAY_OK;
}

galay_status_t galay_postgres_config_set_tcp_no_delay(galay_postgres_config_t* config,
                                                      galay_bool_t enabled)
{
    if (config == nullptr || (enabled != GALAY_FALSE && enabled != GALAY_TRUE)) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->tcp_no_delay = enabled == GALAY_TRUE;
    return GALAY_OK;
}

galay_status_t galay_postgres_config_validate(const galay_postgres_config_t* config)
{
    return validateConfig(config);
}

void galay_postgres_buffer_destroy(galay_postgres_buffer_t* buffer) { delete buffer; }

galay_status_t galay_postgres_buffer_data(const galay_postgres_buffer_t* buffer,
                                          const unsigned char** data,
                                          size_t* data_len)
{
    if (buffer == nullptr || data == nullptr || data_len == nullptr) return GALAY_INVALID_ARGUMENT;
    *data = reinterpret_cast<const unsigned char*>(buffer->data.data());
    *data_len = buffer->data.size();
    return GALAY_OK;
}

galay_status_t galay_postgres_parse_message_header(const unsigned char* data,
                                                   size_t data_len,
                                                   galay_postgres_message_header_t* header)
{
    if (data == nullptr || header == nullptr) return GALAY_INVALID_ARGUMENT;
    if (data_len < kHeaderSize) return GALAY_PROTOCOL_ERROR;
    const uint32_t length = readU32(data + 1);
    if (length < kLengthSize || length > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return GALAY_PROTOCOL_ERROR;
    }
    header->type = static_cast<char>(data[0]);
    header->length = length;
    return GALAY_OK;
}

galay_status_t galay_postgres_extract_message(const unsigned char* data,
                                              size_t data_len,
                                              galay_postgres_message_view_t* view)
{
    if (data == nullptr || view == nullptr) return GALAY_INVALID_ARGUMENT;
    galay_postgres_message_header_t header{};
    const galay_status_t status = galay_postgres_parse_message_header(data, data_len, &header);
    if (status != GALAY_OK) return status;
    const size_t total = 1U + static_cast<size_t>(header.length);
    if (data_len < total) return GALAY_PROTOCOL_ERROR;
    view->type = header.type;
    view->payload = data + kHeaderSize;
    view->payload_len = header.length - static_cast<uint32_t>(kLengthSize);
    view->consumed = total;
    return GALAY_OK;
}

galay_status_t galay_postgres_encode_startup_message(const galay_postgres_config_t* config,
                                                     galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (validateConfig(config) != GALAY_OK || out == nullptr || hasNull(config->username) ||
        hasNull(config->database) || hasNull(config->application_name)) return GALAY_INVALID_ARGUMENT;
    std::string payload;
    writeU32(payload, 196608);
    writeCString(payload, "user");
    writeCString(payload, config->username);
    if (!config->database.empty()) {
        writeCString(payload, "database");
        writeCString(payload, config->database);
    }
    if (!config->application_name.empty()) {
        writeCString(payload, "application_name");
        writeCString(payload, config->application_name);
    }
    payload.push_back('\0');
    const size_t total = kLengthSize + payload.size();
    if (total > kMaxStartupLength) return GALAY_INVALID_ARGUMENT;
    std::string message;
    writeU32(message, static_cast<uint32_t>(total));
    message.append(payload);
    return makeBuffer(std::move(message), out);
}

galay_status_t galay_postgres_encode_sasl_initial_response(const char* mechanism,
                                                           const char* client_first,
                                                           galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (mechanism == nullptr || mechanism[0] == '\0' || client_first == nullptr || out == nullptr ||
        hasNull(mechanism)) return GALAY_INVALID_ARGUMENT;
    std::string payload;
    writeCString(payload, mechanism);
    const size_t length = std::strlen(client_first);
    if (length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return GALAY_INVALID_ARGUMENT;
    writeU32(payload, static_cast<uint32_t>(length));
    payload.append(client_first, length);
    return makeBuffer(wrapMessage('p', payload), out);
}

galay_status_t galay_postgres_encode_sasl_response(const char* client_final,
                                                   galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (client_final == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    return makeBuffer(wrapMessage('p', client_final), out);
}

galay_status_t galay_postgres_encode_password_message(const char* password,
                                                      galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (password == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    std::string payload(password);
    payload.push_back('\0');
    return makeBuffer(wrapMessage('p', payload), out);
}

galay_status_t galay_postgres_encode_query(const char* sql, galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (sql == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT;
    return makeBuffer(encodeQuery(sql), out);
}

galay_status_t galay_postgres_encode_terminate(galay_postgres_buffer_t** out)
{
    return makeBuffer(wrapMessage('X', {}), out);
}

galay_status_t galay_postgres_encode_parse(const char* statement_name,
                                           const char* sql,
                                           const uint32_t* parameter_type_oids,
                                           size_t parameter_type_count,
                                           galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (statement_name == nullptr || sql == nullptr || out == nullptr ||
        (parameter_type_count != 0 && parameter_type_oids == nullptr)) return GALAY_INVALID_ARGUMENT;
    return makeBuffer(encodeParse(statement_name, sql, parameter_type_oids, parameter_type_count), out);
}

galay_status_t galay_postgres_encode_bind(const char* portal_name,
                                          const char* statement_name,
                                          const galay_postgres_stmt_bind_t* binds,
                                          size_t bind_count,
                                          galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (portal_name == nullptr || statement_name == nullptr || out == nullptr ||
        hasNull(portal_name) || hasNull(statement_name) ||
        bind_count > static_cast<size_t>(std::numeric_limits<int16_t>::max()) ||
        (bind_count != 0 && binds == nullptr)) return GALAY_INVALID_ARGUMENT;
    std::string payload;
    writeCString(payload, portal_name);
    writeCString(payload, statement_name);
    writeU16(payload, 0);
    writeU16(payload, static_cast<uint16_t>(bind_count));
    for (size_t index = 0; index < bind_count; ++index) {
        if (binds[index].is_null == GALAY_TRUE) {
            writeU32(payload, std::numeric_limits<uint32_t>::max());
            continue;
        }
        if (binds[index].is_null != GALAY_FALSE ||
            (binds[index].data == nullptr && binds[index].data_len != 0) ||
            binds[index].data_len > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return GALAY_INVALID_ARGUMENT;
        }
        writeU32(payload, static_cast<uint32_t>(binds[index].data_len));
        if (binds[index].data_len != 0) {
            payload.append(reinterpret_cast<const char*>(binds[index].data), binds[index].data_len);
        }
    }
    writeU16(payload, 0);
    return makeBuffer(wrapMessage('B', payload), out);
}

#define GALAY_PG_NAMED_ENCODER(name, type, discriminator) \
    galay_status_t name(const char* value, galay_postgres_buffer_t** out) \
    { \
        if (out != nullptr) *out = nullptr; \
        if (value == nullptr || out == nullptr) return GALAY_INVALID_ARGUMENT; \
        return makeBuffer(encodeNamedMessage(type, discriminator, value), out); \
    }

GALAY_PG_NAMED_ENCODER(galay_postgres_encode_describe_statement, 'D', 'S')
GALAY_PG_NAMED_ENCODER(galay_postgres_encode_describe_portal, 'D', 'P')
GALAY_PG_NAMED_ENCODER(galay_postgres_encode_close_statement, 'C', 'S')
GALAY_PG_NAMED_ENCODER(galay_postgres_encode_close_portal, 'C', 'P')
#undef GALAY_PG_NAMED_ENCODER

galay_status_t galay_postgres_encode_execute(const char* portal_name,
                                             uint32_t max_rows,
                                             galay_postgres_buffer_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (portal_name == nullptr || out == nullptr || hasNull(portal_name) ||
        max_rows > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string payload;
    writeCString(payload, portal_name);
    writeU32(payload, max_rows);
    return makeBuffer(wrapMessage('E', payload), out);
}

galay_status_t galay_postgres_encode_sync(galay_postgres_buffer_t** out)
{
    return makeBuffer(wrapMessage('S', {}), out);
}

galay_status_t galay_postgres_result_set_decode(const unsigned char* data,
                                                size_t data_len,
                                                galay_postgres_result_set_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (data == nullptr || data_len == 0 || out == nullptr) return GALAY_INVALID_ARGUMENT;
    auto* result = new (std::nothrow) galay_postgres_result_set_t();
    if (result == nullptr) return GALAY_OUT_OF_MEMORY;
    const galay_status_t status = parseResult(data, data_len, &result->value);
    if (status != GALAY_OK) {
        delete result;
        return status;
    }
    *out = result;
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_create(galay_postgres_result_set_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_postgres_result_set_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

galay_status_t galay_postgres_result_set_reset(galay_postgres_result_set_t* result)
{
    if (result == nullptr) return GALAY_INVALID_ARGUMENT;
    resetResultSet(&result->value);
    return GALAY_OK;
}

void galay_postgres_result_set_destroy(galay_postgres_result_set_t* result) { delete result; }

galay_status_t galay_postgres_result_set_field_count(const galay_postgres_result_set_t* result,
                                                     size_t* count)
{
    if (result == nullptr || count == nullptr) return GALAY_INVALID_ARGUMENT;
    *count = result->value.field_count;
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_row_count(const galay_postgres_result_set_t* result,
                                                   size_t* count)
{
    if (result == nullptr || count == nullptr) return GALAY_INVALID_ARGUMENT;
    *count = result->value.row_count;
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_field(const galay_postgres_result_set_t* result,
                                               size_t index,
                                               galay_postgres_field_view_t* field)
{
    if (result == nullptr || field == nullptr) return GALAY_INVALID_ARGUMENT;
    if (index >= result->value.field_count) return GALAY_NOT_FOUND;
    fieldToView(result->value.fields[index], field);
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_find_field(const galay_postgres_result_set_t* result,
                                                    const char* name,
                                                    size_t* index)
{
    if (result == nullptr || name == nullptr || index == nullptr) return GALAY_INVALID_ARGUMENT;
    for (size_t position = 0; position < result->value.field_count; ++position) {
        if (result->value.fields[position].name == name) {
            *index = position;
            return GALAY_OK;
        }
    }
    return GALAY_NOT_FOUND;
}

galay_status_t galay_postgres_result_set_value(const galay_postgres_result_set_t* result,
                                               size_t row,
                                               size_t column,
                                               galay_postgres_value_view_t* value)
{
    if (result == nullptr || value == nullptr) return GALAY_INVALID_ARGUMENT;
    if (row >= result->value.row_count || column >= result->value.field_count ||
        column >= result->value.rows[row].size()) return GALAY_NOT_FOUND;
    const auto& source = result->value.rows[row][column];
    value->is_null = source ? GALAY_FALSE : GALAY_TRUE;
    value->data = source ? reinterpret_cast<const unsigned char*>(source->data()) : nullptr;
    value->data_len = source ? source->size() : 0;
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_command_tag(const galay_postgres_result_set_t* result,
                                                     const char** tag)
{
    if (result == nullptr || tag == nullptr) return GALAY_INVALID_ARGUMENT;
    *tag = result->value.command_tag.c_str();
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_affected_rows(const galay_postgres_result_set_t* result,
                                                       uint64_t* affected_rows)
{
    if (result == nullptr || affected_rows == nullptr) return GALAY_INVALID_ARGUMENT;
    *affected_rows = result->value.affected_rows;
    return GALAY_OK;
}

galay_status_t galay_postgres_result_set_transaction_status(
    const galay_postgres_result_set_t* result, char* status)
{
    if (result == nullptr || status == nullptr) return GALAY_INVALID_ARGUMENT;
    *status = result->value.transaction_status;
    return GALAY_OK;
}

galay_status_t galay_postgres_stmt_create(const char* name, galay_postgres_stmt_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (name == nullptr || name[0] == '\0' || out == nullptr) return GALAY_INVALID_ARGUMENT;
    auto* stmt = new (std::nothrow) galay_postgres_stmt_t();
    if (stmt == nullptr) return GALAY_OUT_OF_MEMORY;
    stmt->name = name;
    *out = stmt;
    return GALAY_OK;
}

void galay_postgres_stmt_destroy(galay_postgres_stmt_t* stmt) { delete stmt; }

galay_status_t galay_postgres_stmt_name(const galay_postgres_stmt_t* stmt, const char** name)
{
    if (stmt == nullptr || name == nullptr) return GALAY_INVALID_ARGUMENT;
    *name = stmt->name.c_str();
    return GALAY_OK;
}

galay_status_t galay_postgres_stmt_param_count(const galay_postgres_stmt_t* stmt, size_t* count)
{
    if (stmt == nullptr || count == nullptr) return GALAY_INVALID_ARGUMENT;
    *count = stmt->parameter_types.size();
    return GALAY_OK;
}

galay_status_t galay_postgres_stmt_column_count(const galay_postgres_stmt_t* stmt, size_t* count)
{
    if (stmt == nullptr || count == nullptr) return GALAY_INVALID_ARGUMENT;
    *count = stmt->fields.size();
    return GALAY_OK;
}

galay_status_t galay_postgres_stmt_field(const galay_postgres_stmt_t* stmt,
                                         size_t index,
                                         galay_postgres_field_view_t* field)
{
    if (stmt == nullptr || field == nullptr) return GALAY_INVALID_ARGUMENT;
    if (index >= stmt->fields.size()) return GALAY_NOT_FOUND;
    fieldToView(stmt->fields[index], field);
    return GALAY_OK;
}

galay_status_t galay_postgres_pipeline_create(galay_postgres_pipeline_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_postgres_pipeline_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_postgres_pipeline_destroy(galay_postgres_pipeline_t* pipeline) { delete pipeline; }

galay_status_t galay_postgres_pipeline_append_query(galay_postgres_pipeline_t* pipeline,
                                                    const char* sql)
{
    if (pipeline == nullptr || sql == nullptr) return GALAY_INVALID_ARGUMENT;
    std::string encoded = encodeQuery(sql);
    if (encoded.empty()) return GALAY_INVALID_ARGUMENT;
    pipeline->commands.push_back(PipelineCommand{std::move(encoded), true});
    return GALAY_OK;
}

galay_status_t galay_postgres_pipeline_append_parse(galay_postgres_pipeline_t* pipeline,
                                                    const char* statement_name,
                                                    const char* sql)
{
    if (pipeline == nullptr || statement_name == nullptr || sql == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::string encoded = encodeParse(statement_name, sql, nullptr, 0);
    if (encoded.empty()) return GALAY_INVALID_ARGUMENT;
    pipeline->commands.push_back(PipelineCommand{std::move(encoded), false});
    return GALAY_OK;
}

galay_status_t galay_postgres_pipeline_append_sync(galay_postgres_pipeline_t* pipeline)
{
    if (pipeline == nullptr) return GALAY_INVALID_ARGUMENT;
    pipeline->commands.push_back(PipelineCommand{wrapMessage('S', {}), true});
    return GALAY_OK;
}

galay_status_t galay_postgres_pipeline_build(const galay_postgres_pipeline_t* pipeline,
                                             galay_postgres_buffer_t** out,
                                             size_t* expected_ready)
{
    if (out != nullptr) *out = nullptr;
    if (pipeline == nullptr || out == nullptr || expected_ready == nullptr ||
        pipeline->commands.empty()) return GALAY_INVALID_ARGUMENT;
    std::string encoded;
    size_t ready = 0;
    for (const auto& command : pipeline->commands) {
        encoded.append(command.encoded);
        if (command.ready) ++ready;
    }
    *expected_ready = ready;
    return makeBuffer(std::move(encoded), out);
}

galay_status_t galay_postgres_client_create(galay_postgres_client_t** out)
{
    if (out == nullptr) return GALAY_INVALID_ARGUMENT;
    *out = new (std::nothrow) galay_postgres_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_postgres_client_destroy(galay_postgres_client_t* client)
{
    if (!beginClientOperation(client)) return;
    (void)closeClientStorage(client);
    finishClientOperation(client);
    delete client;
}

void galay_postgres_client_close(galay_postgres_client_t* client)
{
    if (!beginClientOperation(client)) return;
    (void)closeClientStorage(client);
    finishClientOperation(client);
}

galay_status_t galay_postgres_client_is_connected(const galay_postgres_client_t* client,
                                                  galay_bool_t* connected)
{
    if (client == nullptr || connected == nullptr) return GALAY_INVALID_ARGUMENT;
    *connected = client->connected ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_postgres_client_connect(galay_postgres_client_t* client,
                                             const galay_postgres_config_t* config)
{
    if (client == nullptr || validateConfig(config) != GALAY_OK || client->connected) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_UNSUPPORTED;
}

C_IOResult galay_postgres_client_connect_async(galay_postgres_client_t* client,
                                               const galay_postgres_config_t* config,
                                               int64_t timeout_ms)
{
    if (client == nullptr || validateConfig(config) != GALAY_OK || client->connected ||
        client->socket.socket != nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult wait_initialized = initializeClientWaitState(client);
    if (wait_initialized.code != C_IOResultOk) {
        finishClientOperation(client);
        return wait_initialized;
    }
    C_IOResult result = connectAndAuthenticate(client, config, timeout_ms);
    if (result.code != C_IOResultOk) {
        bool cleanup_failed = false;
        if (client->socket.socket != nullptr) {
            const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
            cleanup_failed = destroyed != C_TcpSocketSuccess;
        }
        clearClientProtocolState(client);
        C_IOResult wait_destroyed = destroyClientWaitState(client);
        cleanup_failed = cleanup_failed || wait_destroyed.code != C_IOResultOk;
        if (cleanup_failed) result = statusResult(GALAY_IO_ERROR);
    }
    finishClientOperation(client);
    return result;
}

C_IOResult galay_postgres_client_query_async(galay_postgres_client_t* client,
                                             const char* sql,
                                             int64_t timeout_ms,
                                             galay_postgres_result_set_t** result)
{
    if (result != nullptr) *result = nullptr;
    if (client == nullptr || sql == nullptr || result == nullptr || !client->connected ||
        client->socket.socket == nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult io = queryResult(client, sql, timeout_ms, result);
    finishClientOperation(client);
    return io;
}

C_IOResult galay_postgres_client_query_result_async(galay_postgres_client_t* client,
                                                    const char* sql,
                                                    int64_t timeout_ms,
                                                    galay_postgres_result_set_t** result)
{
    return galay_postgres_client_query_async(client, sql, timeout_ms, result);
}

C_IOResult galay_postgres_client_query_into_async(galay_postgres_client_t* client,
                                                  const char* sql,
                                                  int64_t timeout_ms,
                                                  galay_postgres_result_set_t* result)
{
    if (client == nullptr || sql == nullptr || result == nullptr || !client->connected ||
        client->socket.socket == nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult io = queryResultInto(client, sql, timeout_ms, result);
    finishClientOperation(client);
    return io;
}

C_IOResult galay_postgres_client_begin_transaction_async(
    galay_postgres_client_t* client,
    int64_t timeout_ms,
    galay_postgres_result_set_t** result)
{
    return galay_postgres_client_query_async(client, "BEGIN", timeout_ms, result);
}

C_IOResult galay_postgres_client_commit_async(galay_postgres_client_t* client,
                                              int64_t timeout_ms,
                                              galay_postgres_result_set_t** result)
{
    return galay_postgres_client_query_async(client, "COMMIT", timeout_ms, result);
}

C_IOResult galay_postgres_client_rollback_async(galay_postgres_client_t* client,
                                                int64_t timeout_ms,
                                                galay_postgres_result_set_t** result)
{
    return galay_postgres_client_query_async(client, "ROLLBACK", timeout_ms, result);
}

static C_IOResult prepareStatement(galay_postgres_client_t* client,
                                   const char* statement_name,
                                   const char* sql,
                                   int64_t timeout_ms,
                                   galay_postgres_stmt_t** stmt)
{
    galay_postgres_buffer_t* parse = nullptr;
    galay_postgres_buffer_t* describe = nullptr;
    galay_postgres_buffer_t* sync = nullptr;
    galay_status_t status = galay_postgres_encode_parse(statement_name, sql, nullptr, 0, &parse);
    if (status == GALAY_OK) {
        status = galay_postgres_encode_describe_statement(statement_name, &describe);
    }
    if (status == GALAY_OK) status = galay_postgres_encode_sync(&sync);
    if (status != GALAY_OK) {
        galay_postgres_buffer_destroy(parse);
        galay_postgres_buffer_destroy(describe);
        galay_postgres_buffer_destroy(sync);
        return statusResult(status);
    }
    std::string command;
    command.reserve(parse->data.size() + describe->data.size() + sync->data.size());
    command.append(parse->data);
    command.append(describe->data);
    command.append(sync->data);
    galay_postgres_buffer_destroy(parse);
    galay_postgres_buffer_destroy(describe);
    galay_postgres_buffer_destroy(sync);
    C_IOResult sent = socketWriteAll(client, command, timeout_ms);
    if (sent.code != C_IOResultOk) return sent;

    auto* prepared = new (std::nothrow) galay_postgres_stmt_t();
    if (prepared == nullptr) return statusResult(GALAY_OUT_OF_MEMORY);
    prepared->name = statement_name;
    C_IOResult read = readPreparedMetadata(client, timeout_ms, prepared);
    if (read.code != C_IOResultOk) {
        delete prepared;
        return read;
    }
    *stmt = prepared;
    read.ptr = prepared;
    return read;
}

static C_IOResult executeStatement(
    galay_postgres_client_t* client,
    const galay_postgres_stmt_t* stmt,
    const galay_postgres_stmt_bind_t* binds,
    size_t bind_count,
    int64_t timeout_ms,
    galay_postgres_result_set_t** result)
{
    galay_postgres_buffer_t* bind = nullptr;
    galay_postgres_buffer_t* describe = nullptr;
    galay_postgres_buffer_t* execute = nullptr;
    galay_postgres_buffer_t* sync = nullptr;
    galay_status_t status = galay_postgres_encode_bind("", stmt->name.c_str(), binds,
                                                       bind_count, &bind);
    if (status == GALAY_OK) status = galay_postgres_encode_describe_portal("", &describe);
    if (status == GALAY_OK) status = galay_postgres_encode_execute("", 0, &execute);
    if (status == GALAY_OK) status = galay_postgres_encode_sync(&sync);
    if (status != GALAY_OK) {
        galay_postgres_buffer_destroy(bind);
        galay_postgres_buffer_destroy(describe);
        galay_postgres_buffer_destroy(execute);
        galay_postgres_buffer_destroy(sync);
        return statusResult(status);
    }
    std::string command;
    command.reserve(bind->data.size() + describe->data.size() + execute->data.size() +
                    sync->data.size());
    command.append(bind->data);
    command.append(describe->data);
    command.append(execute->data);
    command.append(sync->data);
    galay_postgres_buffer_destroy(bind);
    galay_postgres_buffer_destroy(describe);
    galay_postgres_buffer_destroy(execute);
    galay_postgres_buffer_destroy(sync);
    C_IOResult sent = socketWriteAll(client, command, timeout_ms);
    if (sent.code != C_IOResultOk) return sent;

    std::string response;
    bool ready = false;
    do {
        std::string_view encoded_message;
        C_IOResult read = socketReadMessage(client, &encoded_message, timeout_ms);
        if (read.code != C_IOResultOk) return read;
        galay_postgres_message_view_t message{};
        status = galay_postgres_extract_message(
            reinterpret_cast<const unsigned char*>(encoded_message.data()),
            encoded_message.size(), &message);
        if (status != GALAY_OK) return statusResult(status);
        response.append(encoded_message);
        ready = message.type == 'Z';
    } while (!ready);
    auto* decoded = new (std::nothrow) galay_postgres_result_set_t();
    if (decoded == nullptr) return statusResult(GALAY_OUT_OF_MEMORY);
    status = parseResult(reinterpret_cast<const unsigned char*>(response.data()),
                         response.size(), &decoded->value);
    if (status != GALAY_OK) {
        delete decoded;
        return statusResult(status);
    }
    *result = decoded;
    C_IOResult io = ioResult(C_IOResultOk);
    io.bytes = response.size();
    io.ptr = decoded;
    return io;
}

static C_IOResult executePipeline(galay_postgres_client_t* client,
                                  const galay_postgres_pipeline_t* pipeline,
                                  int64_t timeout_ms,
                                  galay_postgres_pipeline_result_t** result)
{
    std::string command;
    size_t expected_ready = 0;
    for (const auto& item : pipeline->commands) {
        command.append(item.encoded);
        if (item.ready) ++expected_ready;
    }
    if (expected_ready == 0) return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    C_IOResult sent = socketWriteAll(client, command, timeout_ms);
    if (sent.code != C_IOResultOk) return sent;
    auto* pipeline_result = new (std::nothrow) galay_postgres_pipeline_result_t();
    if (pipeline_result == nullptr) return statusResult(GALAY_OUT_OF_MEMORY);
    pipeline_result->results.reserve(expected_ready);
    C_IOResult read = readPipelineResults(client, expected_ready, timeout_ms, pipeline_result);
    if (read.code != C_IOResultOk) {
        delete pipeline_result;
        return read;
    }
    *result = pipeline_result;
    read.ptr = pipeline_result;
    return read;
}

C_IOResult galay_postgres_client_stmt_prepare_async(galay_postgres_client_t* client,
                                                    const char* statement_name,
                                                    const char* sql,
                                                    int64_t timeout_ms,
                                                    galay_postgres_stmt_t** stmt)
{
    if (stmt != nullptr) *stmt = nullptr;
    if (client == nullptr || statement_name == nullptr || statement_name[0] == '\0' ||
        sql == nullptr || stmt == nullptr || !client->connected ||
        client->socket.socket == nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult result = prepareStatement(client, statement_name, sql, timeout_ms, stmt);
    finishClientOperation(client);
    return result;
}

C_IOResult galay_postgres_client_stmt_execute_async(
    galay_postgres_client_t* client,
    const galay_postgres_stmt_t* stmt,
    const galay_postgres_stmt_bind_t* binds,
    size_t bind_count,
    int64_t timeout_ms,
    galay_postgres_result_set_t** result)
{
    if (result != nullptr) *result = nullptr;
    if (client == nullptr || stmt == nullptr || result == nullptr || !client->connected ||
        client->socket.socket == nullptr || bind_count != stmt->parameter_types.size() ||
        (bind_count != 0 && binds == nullptr)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult io = executeStatement(client, stmt, binds, bind_count, timeout_ms, result);
    finishClientOperation(client);
    return io;
}

C_IOResult galay_postgres_client_pipeline_async(galay_postgres_client_t* client,
                                                const galay_postgres_pipeline_t* pipeline,
                                                int64_t timeout_ms,
                                                galay_postgres_pipeline_result_t** result)
{
    if (result != nullptr) *result = nullptr;
    if (client == nullptr || pipeline == nullptr || pipeline->commands.empty() ||
        result == nullptr || !client->connected || client->socket.socket == nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult io = executePipeline(client, pipeline, timeout_ms, result);
    finishClientOperation(client);
    return io;
}

void galay_postgres_pipeline_result_destroy(galay_postgres_pipeline_result_t* result)
{
    delete result;
}

galay_status_t galay_postgres_pipeline_result_count(
    const galay_postgres_pipeline_result_t* result, size_t* count)
{
    if (result == nullptr || count == nullptr) return GALAY_INVALID_ARGUMENT;
    *count = result->results.size();
    return GALAY_OK;
}

galay_status_t galay_postgres_pipeline_result_at(
    const galay_postgres_pipeline_result_t* result,
    size_t index,
    const galay_postgres_result_set_t** item)
{
    if (result == nullptr || item == nullptr) return GALAY_INVALID_ARGUMENT;
    if (index >= result->results.size()) return GALAY_NOT_FOUND;
    *item = &result->results[index];
    return GALAY_OK;
}

static C_IOResult closeClientAsync(galay_postgres_client_t* client, int64_t timeout_ms)
{
    galay_postgres_buffer_t* terminate = nullptr;
    const galay_status_t encoded = galay_postgres_encode_terminate(&terminate);
    if (encoded != GALAY_OK) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        clearClientProtocolState(client);
        C_IOResult wait_destroyed = destroyClientWaitState(client);
        return destroyed == C_TcpSocketSuccess && wait_destroyed.code == C_IOResultOk
            ? statusResult(encoded) : statusResult(GALAY_IO_ERROR);
    }
    C_IOResult sent = socketWriteAll(client, terminate->data, timeout_ms);
    galay_postgres_buffer_destroy(terminate);
    C_IOResult closed = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    clearClientProtocolState(client);
    C_IOResult wait_destroyed = destroyClientWaitState(client);
    if (sent.code != C_IOResultOk) return sent;
    if (closed.code != C_IOResultOk) return closed;
    if (destroyed != C_TcpSocketSuccess) return statusResult(GALAY_IO_ERROR);
    if (wait_destroyed.code != C_IOResultOk) return wait_destroyed;
    return ioResult(C_IOResultOk);
}

C_IOResult galay_postgres_client_close_async(galay_postgres_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    if (!beginClientOperation(client)) {
        return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    }
    C_IOResult result = closeClientAsync(client, timeout_ms);
    finishClientOperation(client);
    return result;
}

galay_status_t galay_postgres_pool_create(const galay_postgres_config_t* config,
                                          size_t max_connections,
                                          galay_postgres_pool_t** out)
{
    if (out != nullptr) *out = nullptr;
    if (validateConfig(config) != GALAY_OK || max_connections == 0 || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* pool = new (std::nothrow) galay_postgres_pool_t();
    if (pool == nullptr) return GALAY_OUT_OF_MEMORY;
    pool->config = *config;
    pool->max_connections = max_connections;
    *out = pool;
    return GALAY_OK;
}

void galay_postgres_pool_destroy(galay_postgres_pool_t* pool)
{
    if (pool == nullptr) return;
    for (auto* client : pool->idle) galay_postgres_client_destroy(client);
    delete pool;
}

C_IOResult galay_postgres_pool_acquire_async(galay_postgres_pool_t* pool,
                                             int64_t timeout_ms,
                                             galay_postgres_pool_lease_t** lease)
{
    if (lease != nullptr) *lease = nullptr;
    if (pool == nullptr || lease == nullptr) return ioResult(C_IOResultInvalid, GALAY_INVALID_ARGUMENT);
    galay_postgres_client_t* client = nullptr;
    if (!pool->idle.empty()) {
        client = pool->idle.back();
        pool->idle.pop_back();
    } else {
        if (pool->total_connections >= pool->max_connections) {
            return statusResult(GALAY_UNSUPPORTED);
        }
        const galay_status_t created = galay_postgres_client_create(&client);
        if (created != GALAY_OK) return statusResult(created);
        ++pool->total_connections;
        C_IOResult connected = galay_postgres_client_connect_async(client, &pool->config, timeout_ms);
        if (connected.code != C_IOResultOk) {
            galay_postgres_client_destroy(client);
            --pool->total_connections;
            return connected;
        }
    }
    auto* acquired = new (std::nothrow) galay_postgres_pool_lease_t();
    if (acquired == nullptr) {
        pool->idle.push_back(client);
        return statusResult(GALAY_OUT_OF_MEMORY);
    }
    acquired->pool = pool;
    acquired->client = client;
    *lease = acquired;
    C_IOResult result = ioResult(C_IOResultOk);
    result.ptr = acquired;
    return result;
}

galay_status_t galay_postgres_pool_lease_client(galay_postgres_pool_lease_t* lease,
                                                galay_postgres_client_t** client)
{
    if (lease == nullptr || lease->pool == nullptr || lease->client == nullptr || client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *client = lease->client;
    return GALAY_OK;
}

galay_status_t galay_postgres_pool_lease_release(galay_postgres_pool_lease_t* lease)
{
    if (lease == nullptr || lease->pool == nullptr || lease->client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    lease->pool->idle.push_back(lease->client);
    lease->client = nullptr;
    lease->pool = nullptr;
    delete lease;
    return GALAY_OK;
}

} // extern "C"
