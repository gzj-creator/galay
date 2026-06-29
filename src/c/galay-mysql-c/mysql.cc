#include <galay/c/galay-mysql-c/mysql.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket_c.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr uint32_t kMysqlMaxPacketPayload = 1024 * 1024;
constexpr uint32_t kMysqlWireMaxPacketPayload = 0x00FFFFFFU;
constexpr uint8_t kMysqlCommandQuery = 0x03;
constexpr uint8_t kMysqlCommandStmtPrepare = 0x16;
constexpr uint8_t kMysqlCommandStmtExecute = 0x17;
constexpr uint32_t kMysqlCapabilityLongPassword = 0x00000001U;
constexpr uint32_t kMysqlCapabilityConnectWithDb = 0x00000008U;
constexpr uint32_t kMysqlCapabilityProtocol41 = 0x00000200U;
constexpr uint32_t kMysqlCapabilityTransactions = 0x00002000U;
constexpr uint32_t kMysqlCapabilitySecureConnection = 0x00008000U;
constexpr uint32_t kMysqlCapabilityPluginAuth = 0x00080000U;

struct MysqlResultField {
    std::string catalog;
    std::string schema;
    std::string table;
    std::string org_table;
    std::string name;
    std::string org_name;
    uint16_t character_set = 0;
    uint32_t column_length = 0;
    uint8_t column_type = 0;
    uint16_t flags = 0;
    uint8_t decimals = 0;
};

struct MysqlResultValue {
    bool is_null = false;
    std::string data;
};

struct MysqlHandshakeInfo {
    std::string auth_plugin_name;
    std::string auth_plugin_data;
    uint32_t capability_flags = 0;
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

C_IOResult io_result_from_socket_create(C_TcpSocketResultCode code)
{
    return make_io_result(code == C_TcpSocketParameterInvalid ? C_IOResultInvalid : C_IOResultError,
                          static_cast<int64_t>(code));
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
                             unsigned char* data,
                             size_t data_len,
                             int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t received = 0;
    while (received < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_recv(socket,
                                                         reinterpret_cast<char*>(data + received),
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
                              const unsigned char* data,
                              size_t data_len,
                              int64_t timeout_ms)
{
    if (socket == nullptr || data == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    size_t sent = 0;
    while (sent < data_len) {
        C_IOResult result = galay_kernel_tcp_socket_send(socket,
                                                         reinterpret_cast<const char*>(data + sent),
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

uint32_t mysql_payload_length(const unsigned char* header)
{
    return static_cast<uint32_t>(header[0]) |
        (static_cast<uint32_t>(header[1]) << 8U) |
        (static_cast<uint32_t>(header[2]) << 16U);
}

uint16_t mysql_read_u16(const unsigned char* data)
{
    return static_cast<uint16_t>(data[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t mysql_read_u32(const unsigned char* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) |
        (static_cast<uint32_t>(data[2]) << 16U) |
        (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t mysql_read_u64(const unsigned char* data)
{
    return static_cast<uint64_t>(mysql_read_u32(data)) |
        (static_cast<uint64_t>(mysql_read_u32(data + 4)) << 32U);
}

void mysql_write_u16(std::vector<unsigned char>& out, uint16_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
}

void mysql_write_u24(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
}

void mysql_write_u32(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
}

void mysql_write_lenenc_int(std::vector<unsigned char>& out, uint64_t value)
{
    if (value < 251U) {
        out.push_back(static_cast<unsigned char>(value));
    } else if (value < 0x10000U) {
        out.push_back(0xFC);
        mysql_write_u16(out, static_cast<uint16_t>(value));
    } else if (value < 0x1000000U) {
        out.push_back(0xFD);
        mysql_write_u24(out, static_cast<uint32_t>(value));
    } else {
        out.push_back(0xFE);
        for (uint32_t shift = 0; shift < 64U; shift += 8U) {
            out.push_back(static_cast<unsigned char>((value >> shift) & 0xFFU));
        }
    }
}

void mysql_write_lenenc_string(std::vector<unsigned char>& out,
                               const unsigned char* data,
                               size_t data_len)
{
    mysql_write_lenenc_int(out, data_len);
    if (data_len != 0) {
        out.insert(out.end(), data, data + data_len);
    }
}

galay_status_t mysql_read_lenenc_int(const unsigned char* data,
                                     size_t data_len,
                                     uint64_t* value,
                                     size_t* consumed)
{
    if (data == nullptr || value == nullptr || consumed == nullptr || data_len == 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    const unsigned char first = data[0];
    if (first < 0xFB) {
        *value = first;
        *consumed = 1;
        return GALAY_OK;
    }
    if (first == 0xFB) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (first == 0xFC) {
        if (data_len < 3) {
            return GALAY_PROTOCOL_ERROR;
        }
        *value = mysql_read_u16(data + 1);
        *consumed = 3;
        return GALAY_OK;
    }
    if (first == 0xFD) {
        if (data_len < 4) {
            return GALAY_PROTOCOL_ERROR;
        }
        *value = static_cast<uint32_t>(data[1]) |
            (static_cast<uint32_t>(data[2]) << 8U) |
            (static_cast<uint32_t>(data[3]) << 16U);
        *consumed = 4;
        return GALAY_OK;
    }
    if (data_len < 9) {
        return GALAY_PROTOCOL_ERROR;
    }
    *value = mysql_read_u64(data + 1);
    *consumed = 9;
    return GALAY_OK;
}

galay_status_t mysql_read_lenenc_string(const unsigned char* data,
                                        size_t data_len,
                                        size_t* consumed,
                                        std::string* value)
{
    uint64_t string_len = 0;
    size_t int_consumed = 0;
    if (consumed == nullptr || value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t int_status =
        mysql_read_lenenc_int(data, data_len, &string_len, &int_consumed);
    if (int_status != GALAY_OK) {
        return int_status;
    }
    if (string_len > data_len - int_consumed) {
        return GALAY_PROTOCOL_ERROR;
    }
    value->assign(reinterpret_cast<const char*>(data + int_consumed),
                  static_cast<size_t>(string_len));
    *consumed = int_consumed + static_cast<size_t>(string_len);
    return GALAY_OK;
}

bool mysql_is_eof_payload(const unsigned char* payload, size_t payload_len)
{
    return payload != nullptr && payload_len > 0 && payload[0] == 0xFE && payload_len < 9;
}

bool mysql_is_ok_payload(const unsigned char* payload, size_t payload_len)
{
    return payload != nullptr && payload_len >= 7 && payload[0] == 0x00;
}

bool mysql_is_err_payload(const unsigned char* payload, size_t payload_len)
{
    return payload != nullptr && payload_len >= 3 && payload[0] == 0xFF;
}

bool mysql_is_result_terminator_payload(const unsigned char* payload, size_t payload_len)
{
    return mysql_is_eof_payload(payload, payload_len) || mysql_is_ok_payload(payload, payload_len);
}

void mysql_append_packet_header(std::vector<unsigned char>& out,
                                uint32_t payload_len,
                                uint8_t sequence_id)
{
    mysql_write_u24(out, payload_len);
    out.push_back(sequence_id);
}

std::array<unsigned char, 20> mysql_sha1(const unsigned char* data, size_t data_len)
{
    std::vector<unsigned char> message;
    message.reserve(data_len + 72);
    if (data != nullptr && data_len != 0) {
        message.insert(message.end(), data, data + data_len);
    }
    const uint64_t bit_len = static_cast<uint64_t>(data_len) * 8U;
    message.push_back(0x80);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<unsigned char>((bit_len >> static_cast<uint32_t>(shift)) & 0xFFU));
    }

    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xEFCDAB89U;
    uint32_t h2 = 0x98BADCFEU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xC3D2E1F0U;

    for (size_t chunk = 0; chunk < message.size(); chunk += 64U) {
        uint32_t w[80] = {};
        for (size_t i = 0; i < 16U; ++i) {
            const size_t offset = chunk + i * 4U;
            w[i] = (static_cast<uint32_t>(message[offset]) << 24U) |
                (static_cast<uint32_t>(message[offset + 1]) << 16U) |
                (static_cast<uint32_t>(message[offset + 2]) << 8U) |
                static_cast<uint32_t>(message[offset + 3]);
        }
        for (size_t i = 16U; i < 80U; ++i) {
            const uint32_t value = w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U];
            w[i] = (value << 1U) | (value >> 31U);
        }
        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (size_t i = 0; i < 80U; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20U) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40U) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60U) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const uint32_t rotated = (a << 5U) | (a >> 27U);
            const uint32_t temp = rotated + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30U) | (b >> 2U);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<unsigned char, 20> digest{};
    const uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (size_t i = 0; i < 5U; ++i) {
        digest[i * 4U] = static_cast<unsigned char>((words[i] >> 24U) & 0xFFU);
        digest[i * 4U + 1U] = static_cast<unsigned char>((words[i] >> 16U) & 0xFFU);
        digest[i * 4U + 2U] = static_cast<unsigned char>((words[i] >> 8U) & 0xFFU);
        digest[i * 4U + 3U] = static_cast<unsigned char>(words[i] & 0xFFU);
    }
    return digest;
}

std::vector<unsigned char> mysql_native_password_response(const char* password,
                                                          const unsigned char* salt,
                                                          size_t salt_len)
{
    std::vector<unsigned char> response;
    const size_t password_len = std::strlen(password);
    if (password_len == 0) {
        return response;
    }
    const auto hash1 = mysql_sha1(reinterpret_cast<const unsigned char*>(password), password_len);
    const auto hash2 = mysql_sha1(hash1.data(), hash1.size());
    std::vector<unsigned char> combined;
    combined.reserve(salt_len + hash2.size());
    combined.insert(combined.end(), salt, salt + salt_len);
    combined.insert(combined.end(), hash2.begin(), hash2.end());
    const auto hash3 = mysql_sha1(combined.data(), combined.size());
    response.resize(hash1.size());
    for (size_t i = 0; i < hash1.size(); ++i) {
        response[i] = static_cast<unsigned char>(hash1[i] ^ hash3[i]);
    }
    return response;
}

C_IOResult read_mysql_packet(galay_kernel_tcp_socket_t* socket,
                             std::vector<unsigned char>* packet,
                             int64_t timeout_ms)
{
    if (packet == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    packet->clear();
    unsigned char header[4] = {0, 0, 0, 0};
    C_IOResult header_result = socket_read_exact(socket, header, sizeof(header), timeout_ms);
    if (header_result.code != C_IOResultOk) {
        return header_result;
    }
    const uint32_t payload_len = mysql_payload_length(header);
    if (payload_len > kMysqlMaxPacketPayload) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    packet->resize(sizeof(header) + payload_len);
    std::memcpy(packet->data(), header, sizeof(header));
    if (payload_len != 0) {
        C_IOResult payload_result = socket_read_exact(socket,
                                                      packet->data() + sizeof(header),
                                                      payload_len,
                                                      timeout_ms);
        if (payload_result.code != C_IOResultOk) {
            packet->clear();
            return payload_result;
        }
    }
    C_IOResult result = make_io_result(C_IOResultOk);
    result.bytes = packet->size();
    return result;
}

} // namespace

struct galay_mysql_config_t {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string username = "root";
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    uint32_t timeout_ms = 3000;
};

struct galay_mysql_buffer_t {
    std::vector<unsigned char> data;
};

struct galay_mysql_result_set_t {
    std::vector<MysqlResultField> fields;
    std::vector<std::vector<MysqlResultValue>> rows;
    uint64_t affected_rows = 0;
    uint64_t last_insert_id = 0;
    uint16_t status_flags = 0;
    uint16_t warnings = 0;
};

struct galay_mysql_stmt_t {
    uint32_t statement_id = 0;
    uint16_t num_columns = 0;
    uint16_t num_params = 0;
    uint16_t warnings = 0;
    std::vector<MysqlResultField> param_fields;
    std::vector<MysqlResultField> column_fields;
};

struct galay_mysql_pipeline_t {
    std::vector<std::string> queries;
};

struct galay_mysql_pipeline_result_t {
    std::vector<galay_mysql_result_set_t> results;
};

struct galay_mysql_client_t {
    galay_kernel_tcp_socket_t socket{};
    bool connected = false;
    bool authenticated = false;
    std::vector<unsigned char> handshake_packet;
};

struct galay_mysql_pool_t {
    galay_mysql_config_t config;
    size_t max_connections = 0;
    size_t total_connections = 0;
    std::vector<galay_mysql_client_t*> idle;
};

struct galay_mysql_pool_lease_t {
    galay_mysql_pool_t* pool = nullptr;
    galay_mysql_client_t* client = nullptr;
};

namespace
{

galay_status_t mysql_extract_packet_view_internal(const unsigned char* data,
                                                  size_t data_len,
                                                  galay_mysql_packet_view_t* view)
{
    if (data == nullptr || view == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < 4) {
        return GALAY_PROTOCOL_ERROR;
    }
    const uint32_t payload_len = mysql_payload_length(data);
    if (data_len < 4U + payload_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    view->payload = data + 4;
    view->payload_len = payload_len;
    view->sequence_id = data[3];
    view->consumed = 4U + payload_len;
    return GALAY_OK;
}

void mysql_field_to_view(const MysqlResultField& source, galay_mysql_field_view_t* out)
{
    out->catalog = source.catalog.c_str();
    out->schema = source.schema.c_str();
    out->table = source.table.c_str();
    out->org_table = source.org_table.c_str();
    out->name = source.name.c_str();
    out->org_name = source.org_name.c_str();
    out->character_set = source.character_set;
    out->column_length = source.column_length;
    out->column_type = source.column_type;
    out->flags = source.flags;
    out->decimals = source.decimals;
}

galay_status_t mysql_parse_ok_payload(const unsigned char* payload,
                                      size_t payload_len,
                                      galay_mysql_result_set_t* result)
{
    if (!mysql_is_ok_payload(payload, payload_len) || result == nullptr) {
        return GALAY_PROTOCOL_ERROR;
    }
    size_t pos = 1;
    size_t consumed = 0;
    uint64_t value = 0;
    galay_status_t status = mysql_read_lenenc_int(payload + pos, payload_len - pos, &value, &consumed);
    if (status != GALAY_OK) {
        return status;
    }
    result->affected_rows = value;
    pos += consumed;
    status = mysql_read_lenenc_int(payload + pos, payload_len - pos, &value, &consumed);
    if (status != GALAY_OK) {
        return status;
    }
    result->last_insert_id = value;
    pos += consumed;
    if (pos + 4U > payload_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    result->status_flags = mysql_read_u16(payload + pos);
    pos += 2;
    result->warnings = mysql_read_u16(payload + pos);
    return GALAY_OK;
}

galay_status_t mysql_parse_eof_or_ok_payload(const unsigned char* payload,
                                             size_t payload_len,
                                             galay_mysql_result_set_t* result)
{
    if (mysql_is_ok_payload(payload, payload_len)) {
        return mysql_parse_ok_payload(payload, payload_len, result);
    }
    if (!mysql_is_eof_payload(payload, payload_len)) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (payload_len >= 5 && result != nullptr) {
        result->warnings = mysql_read_u16(payload + 1);
        result->status_flags = mysql_read_u16(payload + 3);
    }
    return GALAY_OK;
}

galay_status_t mysql_parse_column_payload(const unsigned char* payload,
                                          size_t payload_len,
                                          MysqlResultField* field)
{
    if (payload == nullptr || field == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    size_t pos = 0;
    size_t consumed = 0;
    galay_status_t status = mysql_read_lenenc_string(payload + pos,
                                                     payload_len - pos,
                                                     &consumed,
                                                     &field->catalog);
    if (status != GALAY_OK) {
        return status;
    }
    pos += consumed;
    status = mysql_read_lenenc_string(payload + pos, payload_len - pos, &consumed, &field->schema);
    if (status != GALAY_OK) {
        return status;
    }
    pos += consumed;
    status = mysql_read_lenenc_string(payload + pos, payload_len - pos, &consumed, &field->table);
    if (status != GALAY_OK) {
        return status;
    }
    pos += consumed;
    status = mysql_read_lenenc_string(payload + pos, payload_len - pos, &consumed, &field->org_table);
    if (status != GALAY_OK) {
        return status;
    }
    pos += consumed;
    status = mysql_read_lenenc_string(payload + pos, payload_len - pos, &consumed, &field->name);
    if (status != GALAY_OK) {
        return status;
    }
    pos += consumed;
    status = mysql_read_lenenc_string(payload + pos, payload_len - pos, &consumed, &field->org_name);
    if (status != GALAY_OK) {
        return status;
    }
    pos += consumed;
    if (pos + 13U > payload_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    const unsigned char fixed_len = payload[pos++];
    if (fixed_len < 0x0C) {
        return GALAY_PROTOCOL_ERROR;
    }
    field->character_set = mysql_read_u16(payload + pos);
    pos += 2;
    field->column_length = mysql_read_u32(payload + pos);
    pos += 4;
    field->column_type = payload[pos++];
    field->flags = mysql_read_u16(payload + pos);
    pos += 2;
    field->decimals = payload[pos++];
    return GALAY_OK;
}

galay_status_t mysql_parse_row_payload(const unsigned char* payload,
                                       size_t payload_len,
                                       size_t column_count,
                                       std::vector<MysqlResultValue>* row)
{
    if (payload == nullptr || row == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    row->clear();
    row->reserve(column_count);
    size_t pos = 0;
    for (size_t i = 0; i < column_count; ++i) {
        if (pos >= payload_len) {
            return GALAY_PROTOCOL_ERROR;
        }
        MysqlResultValue value;
        if (payload[pos] == 0xFB) {
            value.is_null = true;
            ++pos;
            row->push_back(std::move(value));
            continue;
        }
        uint64_t value_len = 0;
        size_t consumed = 0;
        const galay_status_t status =
            mysql_read_lenenc_int(payload + pos, payload_len - pos, &value_len, &consumed);
        if (status != GALAY_OK) {
            return status;
        }
        pos += consumed;
        if (value_len > payload_len - pos) {
            return GALAY_PROTOCOL_ERROR;
        }
        value.data.assign(reinterpret_cast<const char*>(payload + pos),
                          static_cast<size_t>(value_len));
        pos += static_cast<size_t>(value_len);
        row->push_back(std::move(value));
    }
    return pos == payload_len ? GALAY_OK : GALAY_PROTOCOL_ERROR;
}

galay_status_t mysql_decode_result_packets(const unsigned char* data,
                                           size_t data_len,
                                           galay_mysql_result_set_t* result)
{
    if (data == nullptr || result == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    size_t pos = 0;
    galay_mysql_packet_view_t view{};
    galay_status_t status = mysql_extract_packet_view_internal(data + pos, data_len - pos, &view);
    if (status != GALAY_OK) {
        return status == GALAY_INVALID_ARGUMENT ? GALAY_INVALID_ARGUMENT : GALAY_PROTOCOL_ERROR;
    }
    pos += view.consumed;
    if (view.payload_len == 0 || mysql_is_err_payload(view.payload, view.payload_len)) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (mysql_is_ok_payload(view.payload, view.payload_len)) {
        status = mysql_parse_ok_payload(view.payload, view.payload_len, result);
        return status == GALAY_OK && pos == data_len ? GALAY_OK : GALAY_PROTOCOL_ERROR;
    }

    uint64_t column_count = 0;
    size_t consumed = 0;
    status = mysql_read_lenenc_int(view.payload, view.payload_len, &column_count, &consumed);
    if (status != GALAY_OK || consumed != view.payload_len || column_count == 0) {
        return GALAY_PROTOCOL_ERROR;
    }
    result->fields.reserve(static_cast<size_t>(column_count));
    for (uint64_t i = 0; i < column_count; ++i) {
        status = mysql_extract_packet_view_internal(data + pos, data_len - pos, &view);
        if (status != GALAY_OK || mysql_is_result_terminator_payload(view.payload, view.payload_len) ||
            mysql_is_err_payload(view.payload, view.payload_len)) {
            return GALAY_PROTOCOL_ERROR;
        }
        pos += view.consumed;
        MysqlResultField field;
        status = mysql_parse_column_payload(view.payload, view.payload_len, &field);
        if (status != GALAY_OK) {
            return status;
        }
        result->fields.push_back(std::move(field));
    }

    status = mysql_extract_packet_view_internal(data + pos, data_len - pos, &view);
    if (status != GALAY_OK || !mysql_is_result_terminator_payload(view.payload, view.payload_len)) {
        return GALAY_PROTOCOL_ERROR;
    }
    pos += view.consumed;
    status = mysql_parse_eof_or_ok_payload(view.payload, view.payload_len, result);
    if (status != GALAY_OK) {
        return status;
    }

    while (pos < data_len) {
        status = mysql_extract_packet_view_internal(data + pos, data_len - pos, &view);
        if (status != GALAY_OK) {
            return GALAY_PROTOCOL_ERROR;
        }
        pos += view.consumed;
        if (mysql_is_result_terminator_payload(view.payload, view.payload_len)) {
            status = mysql_parse_eof_or_ok_payload(view.payload, view.payload_len, result);
            return status == GALAY_OK && pos == data_len ? GALAY_OK : GALAY_PROTOCOL_ERROR;
        }
        if (mysql_is_err_payload(view.payload, view.payload_len)) {
            return GALAY_PROTOCOL_ERROR;
        }
        std::vector<MysqlResultValue> row;
        status = mysql_parse_row_payload(view.payload,
                                         view.payload_len,
                                         result->fields.size(),
                                         &row);
        if (status != GALAY_OK) {
            return status;
        }
        result->rows.push_back(std::move(row));
    }
    return GALAY_PROTOCOL_ERROR;
}

galay_status_t mysql_make_simple_packet(uint8_t command,
                                        const unsigned char* payload,
                                        size_t payload_len,
                                        uint8_t sequence_id,
                                        galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (out == nullptr || (payload == nullptr && payload_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (payload_len > kMysqlWireMaxPacketPayload - 1U) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* buffer = new (std::nothrow) galay_mysql_buffer_t();
    if (buffer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    buffer->data.reserve(5U + payload_len);
    mysql_append_packet_header(buffer->data, static_cast<uint32_t>(payload_len + 1U), sequence_id);
    buffer->data.push_back(command);
    if (payload_len != 0) {
        buffer->data.insert(buffer->data.end(), payload, payload + payload_len);
    }
    *out = buffer;
    return GALAY_OK;
}

C_IOResult mysql_send_buffer(galay_mysql_client_t* client,
                             const galay_mysql_buffer_t* buffer,
                             int64_t timeout_ms)
{
    const unsigned char* data = nullptr;
    size_t data_len = 0;
    const galay_status_t status = galay_mysql_buffer_data(buffer, &data, &data_len);
    if (status != GALAY_OK) {
        return io_result_from_status(status);
    }
    return socket_write_exact(&client->socket, data, data_len, timeout_ms);
}

C_IOResult mysql_send_simple_command(galay_mysql_client_t* client,
                                     uint8_t command,
                                     const char* payload,
                                     int64_t timeout_ms)
{
    if (client == nullptr || payload == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_mysql_buffer_t* packet = nullptr;
    const galay_status_t encoded =
        mysql_make_simple_packet(command,
                                 reinterpret_cast<const unsigned char*>(payload),
                                 std::strlen(payload),
                                 0,
                                 &packet);
    if (encoded != GALAY_OK) {
        return io_result_from_status(encoded);
    }
    C_IOResult sent = mysql_send_buffer(client, packet, timeout_ms);
    galay_mysql_buffer_destroy(packet);
    return sent;
}

C_IOResult mysql_read_response_packets(galay_mysql_client_t* client,
                                       int64_t timeout_ms,
                                       std::vector<unsigned char>* packets)
{
    if (client == nullptr || packets == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    packets->clear();
    std::vector<unsigned char> packet;
    C_IOResult read = read_mysql_packet(&client->socket, &packet, timeout_ms);
    if (read.code != C_IOResultOk) {
        return read;
    }
    packets->insert(packets->end(), packet.begin(), packet.end());
    galay_mysql_packet_view_t view{};
    const galay_status_t first_status =
        mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
    if (first_status != GALAY_OK || view.payload_len == 0 || mysql_is_err_payload(view.payload, view.payload_len)) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    if (mysql_is_ok_payload(view.payload, view.payload_len)) {
        read.bytes = packets->size();
        return read;
    }
    uint64_t column_count = 0;
    size_t consumed = 0;
    const galay_status_t count_status =
        mysql_read_lenenc_int(view.payload, view.payload_len, &column_count, &consumed);
    if (count_status != GALAY_OK || consumed != view.payload_len || column_count == 0) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    for (uint64_t i = 0; i < column_count; ++i) {
        read = read_mysql_packet(&client->socket, &packet, timeout_ms);
        if (read.code != C_IOResultOk) {
            return read;
        }
        packets->insert(packets->end(), packet.begin(), packet.end());
    }
    read = read_mysql_packet(&client->socket, &packet, timeout_ms);
    if (read.code != C_IOResultOk) {
        return read;
    }
    packets->insert(packets->end(), packet.begin(), packet.end());
    const galay_status_t terminator_status =
        mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
    if (terminator_status != GALAY_OK ||
        !mysql_is_result_terminator_payload(view.payload, view.payload_len)) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    while (true) {
        read = read_mysql_packet(&client->socket, &packet, timeout_ms);
        if (read.code != C_IOResultOk) {
            return read;
        }
        packets->insert(packets->end(), packet.begin(), packet.end());
        const galay_status_t row_status =
            mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
        if (row_status != GALAY_OK || mysql_is_err_payload(view.payload, view.payload_len)) {
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }
        if (mysql_is_result_terminator_payload(view.payload, view.payload_len)) {
            read.bytes = packets->size();
            return read;
        }
    }
}

C_IOResult mysql_decode_response_packets(const std::vector<unsigned char>& packets,
                                         galay_mysql_result_set_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (out == nullptr || packets.empty()) {
        return make_io_result(C_IOResultInvalid);
    }
    auto* result = new (std::nothrow) galay_mysql_result_set_t();
    if (result == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    const galay_status_t decoded =
        mysql_decode_result_packets(packets.data(), packets.size(), result);
    if (decoded != GALAY_OK) {
        delete result;
        return io_result_from_status(decoded);
    }
    *out = result;
    C_IOResult io = make_io_result(C_IOResultOk);
    io.bytes = packets.size();
    io.ptr = result;
    return io;
}

C_IOResult mysql_client_query_result(galay_mysql_client_t* client,
                                     const char* query,
                                     int64_t timeout_ms,
                                     galay_mysql_result_set_t** result)
{
    if (result != nullptr) {
        *result = nullptr;
    }
    if (client == nullptr || query == nullptr || result == nullptr ||
        !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult sent = mysql_send_simple_command(client, kMysqlCommandQuery, query, timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    std::vector<unsigned char> packets;
    C_IOResult read = mysql_read_response_packets(client, timeout_ms, &packets);
    if (read.code != C_IOResultOk) {
        return read;
    }
    return mysql_decode_response_packets(packets, result);
}

galay_status_t mysql_parse_handshake_info(const std::vector<unsigned char>& packet,
                                          MysqlHandshakeInfo* info)
{
    if (packet.empty() || info == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_mysql_packet_view_t view{};
    galay_status_t status = mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
    if (status != GALAY_OK || view.payload_len < 34 || view.payload[0] != 10) {
        return GALAY_PROTOCOL_ERROR;
    }
    const unsigned char* payload = view.payload;
    size_t pos = 1;
    while (pos < view.payload_len && payload[pos] != '\0') {
        ++pos;
    }
    if (pos >= view.payload_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    ++pos;
    if (pos + 4U + 8U + 1U + 2U > view.payload_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    pos += 4;
    info->auth_plugin_data.assign(reinterpret_cast<const char*>(payload + pos), 8);
    pos += 8;
    ++pos;
    info->capability_flags = mysql_read_u16(payload + pos);
    pos += 2;
    if (pos >= view.payload_len) {
        info->auth_plugin_name = "mysql_native_password";
        return GALAY_OK;
    }
    if (pos + 1U + 2U + 2U + 1U + 10U > view.payload_len) {
        return GALAY_PROTOCOL_ERROR;
    }
    ++pos;
    pos += 2;
    info->capability_flags |= static_cast<uint32_t>(mysql_read_u16(payload + pos)) << 16U;
    pos += 2;
    const unsigned char auth_data_len = payload[pos++];
    pos += 10;
    if ((info->capability_flags & kMysqlCapabilitySecureConnection) != 0) {
        size_t part2_len = 13;
        if (auth_data_len > 8 && static_cast<size_t>(auth_data_len - 8) > part2_len) {
            part2_len = static_cast<size_t>(auth_data_len - 8);
        }
        if (pos + part2_len > view.payload_len) {
            return GALAY_PROTOCOL_ERROR;
        }
        if (part2_len > 0 && payload[pos + part2_len - 1U] == '\0') {
            --part2_len;
        }
        info->auth_plugin_data.append(reinterpret_cast<const char*>(payload + pos), part2_len);
        pos += part2_len;
    }
    if ((info->capability_flags & kMysqlCapabilityPluginAuth) != 0 && pos < view.payload_len) {
        const size_t begin = pos;
        while (pos < view.payload_len && payload[pos] != '\0') {
            ++pos;
        }
        info->auth_plugin_name.assign(reinterpret_cast<const char*>(payload + begin), pos - begin);
    }
    if (info->auth_plugin_name.empty()) {
        info->auth_plugin_name = "mysql_native_password";
    }
    return GALAY_OK;
}

galay_status_t mysql_encode_handshake_response(const galay_mysql_config_t* config,
                                               const MysqlHandshakeInfo& handshake,
                                               galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (config == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (handshake.auth_plugin_name != "mysql_native_password") {
        return GALAY_UNSUPPORTED;
    }
    auto* packet = new (std::nothrow) galay_mysql_buffer_t();
    if (packet == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    std::vector<unsigned char> auth =
        mysql_native_password_response(config->password.c_str(),
                                       reinterpret_cast<const unsigned char*>(handshake.auth_plugin_data.data()),
                                       handshake.auth_plugin_data.size());
    uint32_t capability_flags = kMysqlCapabilityLongPassword |
        kMysqlCapabilityProtocol41 |
        kMysqlCapabilityTransactions |
        kMysqlCapabilitySecureConnection |
        kMysqlCapabilityPluginAuth;
    if (!config->database.empty()) {
        capability_flags |= kMysqlCapabilityConnectWithDb;
    }
    std::vector<unsigned char> payload;
    payload.reserve(64U + config->username.size() + auth.size() +
                    config->database.size() + handshake.auth_plugin_name.size());
    mysql_write_u32(payload, capability_flags);
    mysql_write_u32(payload, kMysqlWireMaxPacketPayload);
    payload.push_back(45);
    payload.insert(payload.end(), 23, 0);
    payload.insert(payload.end(), config->username.begin(), config->username.end());
    payload.push_back(0);
    if (auth.size() > 255U) {
        delete packet;
        return GALAY_PROTOCOL_ERROR;
    }
    payload.push_back(static_cast<unsigned char>(auth.size()));
    payload.insert(payload.end(), auth.begin(), auth.end());
    if (!config->database.empty()) {
        payload.insert(payload.end(), config->database.begin(), config->database.end());
        payload.push_back(0);
    }
    payload.insert(payload.end(), handshake.auth_plugin_name.begin(), handshake.auth_plugin_name.end());
    payload.push_back(0);

    mysql_append_packet_header(packet->data, static_cast<uint32_t>(payload.size()), 1);
    packet->data.insert(packet->data.end(), payload.begin(), payload.end());
    *out = packet;
    return GALAY_OK;
}

galay_status_t mysql_parse_prepare_ok_payload(const unsigned char* payload,
                                              size_t payload_len,
                                              galay_mysql_stmt_t* stmt)
{
    if (payload == nullptr || stmt == nullptr || payload_len < 12 || payload[0] != 0x00) {
        return GALAY_PROTOCOL_ERROR;
    }
    stmt->statement_id = mysql_read_u32(payload + 1);
    stmt->num_columns = mysql_read_u16(payload + 5);
    stmt->num_params = mysql_read_u16(payload + 7);
    stmt->warnings = mysql_read_u16(payload + 10);
    return GALAY_OK;
}

C_IOResult mysql_stmt_read_fields(galay_mysql_client_t* client,
                                  int64_t timeout_ms,
                                  uint16_t count,
                                  std::vector<MysqlResultField>* fields)
{
    std::vector<unsigned char> packet;
    galay_mysql_packet_view_t view{};
    fields->reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        C_IOResult read = read_mysql_packet(&client->socket, &packet, timeout_ms);
        if (read.code != C_IOResultOk) {
            return read;
        }
        const galay_status_t packet_status =
            mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
        if (packet_status != GALAY_OK ||
            mysql_is_result_terminator_payload(view.payload, view.payload_len)) {
            return io_result_from_status(GALAY_PROTOCOL_ERROR);
        }
        MysqlResultField field;
        const galay_status_t field_status =
            mysql_parse_column_payload(view.payload, view.payload_len, &field);
        if (field_status != GALAY_OK) {
            return io_result_from_status(field_status);
        }
        fields->push_back(std::move(field));
    }
    if (count == 0) {
        return make_io_result(C_IOResultOk);
    }
    C_IOResult terminator = read_mysql_packet(&client->socket, &packet, timeout_ms);
    if (terminator.code != C_IOResultOk) {
        return terminator;
    }
    const galay_status_t terminator_status =
        mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
    if (terminator_status != GALAY_OK ||
        !mysql_is_result_terminator_payload(view.payload, view.payload_len)) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    return terminator;
}

galay_status_t mysql_encode_stmt_execute_packet(const galay_mysql_stmt_t* stmt,
                                                const galay_mysql_stmt_bind_t* binds,
                                                size_t bind_count,
                                                galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (stmt == nullptr || out == nullptr || bind_count != stmt->num_params ||
        (bind_count != 0 && binds == nullptr)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* packet = new (std::nothrow) galay_mysql_buffer_t();
    if (packet == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    std::vector<unsigned char> payload;
    payload.reserve(16U + bind_count * 4U);
    payload.push_back(kMysqlCommandStmtExecute);
    mysql_write_u32(payload, stmt->statement_id);
    payload.push_back(0);
    mysql_write_u32(payload, 1);
    if (bind_count != 0) {
        const size_t null_bitmap_len = (bind_count + 7U) / 8U;
        const size_t null_bitmap_pos = payload.size();
        payload.insert(payload.end(), null_bitmap_len, 0);
        for (size_t i = 0; i < bind_count; ++i) {
            if (binds[i].is_null == GALAY_TRUE) {
                payload[null_bitmap_pos + (i / 8U)] |= static_cast<unsigned char>(1U << (i % 8U));
            }
        }
        payload.push_back(1);
        for (size_t i = 0; i < bind_count; ++i) {
            payload.push_back(binds[i].column_type);
            payload.push_back(0);
        }
        for (size_t i = 0; i < bind_count; ++i) {
            if (binds[i].is_null == GALAY_TRUE) {
                continue;
            }
            if (binds[i].data == nullptr && binds[i].data_len != 0) {
                delete packet;
                return GALAY_INVALID_ARGUMENT;
            }
            mysql_write_lenenc_string(payload, binds[i].data, binds[i].data_len);
        }
    }
    mysql_append_packet_header(packet->data, static_cast<uint32_t>(payload.size()), 0);
    packet->data.insert(packet->data.end(), payload.begin(), payload.end());
    *out = packet;
    return GALAY_OK;
}

}

extern "C" {

const char* galay_mysql_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_mysql_config_create(galay_mysql_config_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mysql_config_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mysql_config_destroy(galay_mysql_config_t* config)
{
    delete config;
}

galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config, const char** host)
{
    if (config == nullptr || host == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *host = config->host.c_str();
    return GALAY_OK;
}

galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config, uint16_t* port)
{
    if (config == nullptr || port == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *port = config->port;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config, const char* host)
{
    if (config == nullptr || host == nullptr || host[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    config->host = host;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config, uint16_t port)
{
    if (config == nullptr || port == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->port = port;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config, const char* username)
{
    if (config == nullptr || username == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->username = username;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config, const char* password)
{
    if (config == nullptr || password == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->password = password;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config, const char* database)
{
    if (config == nullptr || database == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->database = database;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config, const char* charset)
{
    if (config == nullptr || charset == nullptr || charset[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    config->charset = charset;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config, uint32_t timeout_ms)
{
    if (config == nullptr || timeout_ms == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    config->timeout_ms = timeout_ms;
    return GALAY_OK;
}

galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config)
{
    return config == nullptr || config->host.empty() || config->port == 0 ? GALAY_INVALID_ARGUMENT : GALAY_OK;
}

galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin, const char* password,
                                                    const unsigned char* salt, size_t salt_len,
                                                    galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (plugin == nullptr || password == nullptr || salt == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (std::strcmp(plugin, "mysql_native_password") != 0) {
        return GALAY_UNSUPPORTED;
    }
    auto* buffer = new (std::nothrow) galay_mysql_buffer_t();
    if (buffer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (salt_len == 0) {
        delete buffer;
        return GALAY_INVALID_ARGUMENT;
    }
    buffer->data = mysql_native_password_response(password, salt, salt_len);
    *out = buffer;
    return GALAY_OK;
}

void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer)
{
    delete buffer;
}

galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                       const unsigned char** data, size_t* data_len)
{
    if (buffer == nullptr || data == nullptr || data_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *data = buffer->data.data();
    *data_len = buffer->data.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_parse_packet_header(const unsigned char* data, size_t data_len,
                                               galay_mysql_packet_header_t* header)
{
    if (data == nullptr || header == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < 4) {
        return GALAY_PROTOCOL_ERROR;
    }
    header->payload_length = static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) | (static_cast<uint32_t>(data[2]) << 16U);
    header->sequence_id = data[3];
    return GALAY_OK;
}

galay_status_t galay_mysql_extract_packet(const unsigned char* data, size_t data_len,
                                          galay_mysql_packet_view_t* view)
{
    if (data == nullptr || view == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    galay_mysql_packet_header_t header;
    const galay_status_t status = galay_mysql_parse_packet_header(data, data_len, &header);
    if (status != GALAY_OK) {
        return status;
    }
    if (data_len < 4 + header.payload_length) {
        return GALAY_PROTOCOL_ERROR;
    }
    view->payload = data + 4;
    view->payload_len = header.payload_length;
    view->sequence_id = header.sequence_id;
    view->consumed = 4 + header.payload_length;
    return GALAY_OK;
}

galay_status_t galay_mysql_encode_query_packet(const char* query, uint8_t sequence_id,
                                               galay_mysql_buffer_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (query == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t query_len = std::strlen(query);
    auto* buffer = new (std::nothrow) galay_mysql_buffer_t();
    if (buffer == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    const uint32_t payload_len = static_cast<uint32_t>(query_len + 1);
    buffer->data.resize(4 + payload_len);
    buffer->data[0] = static_cast<unsigned char>(payload_len & 0xFFU);
    buffer->data[1] = static_cast<unsigned char>((payload_len >> 8U) & 0xFFU);
    buffer->data[2] = static_cast<unsigned char>((payload_len >> 16U) & 0xFFU);
    buffer->data[3] = sequence_id;
    buffer->data[4] = 0x03;
    std::memcpy(buffer->data.data() + 5, query, query_len);
    *out = buffer;
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_decode(const unsigned char* data, size_t data_len,
                                             galay_mysql_result_set_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (data == nullptr || out == nullptr || data_len == 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* result = new (std::nothrow) galay_mysql_result_set_t();
    if (result == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    const galay_status_t decoded = mysql_decode_result_packets(data, data_len, result);
    if (decoded != GALAY_OK) {
        delete result;
        return decoded;
    }
    *out = result;
    return GALAY_OK;
}

void galay_mysql_result_set_destroy(galay_mysql_result_set_t* result)
{
    delete result;
}

galay_status_t galay_mysql_result_set_field_count(const galay_mysql_result_set_t* result,
                                                  size_t* count)
{
    if (result == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = result->fields.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_row_count(const galay_mysql_result_set_t* result,
                                                size_t* count)
{
    if (result == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = result->rows.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_field(const galay_mysql_result_set_t* result,
                                            size_t index,
                                            galay_mysql_field_view_t* field)
{
    if (result == nullptr || field == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->fields.size()) {
        return GALAY_NOT_FOUND;
    }
    mysql_field_to_view(result->fields[index], field);
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_find_field(const galay_mysql_result_set_t* result,
                                                 const char* name,
                                                 size_t* index)
{
    if (result == nullptr || name == nullptr || index == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < result->fields.size(); ++i) {
        if (result->fields[i].name == name) {
            *index = i;
            return GALAY_OK;
        }
    }
    return GALAY_NOT_FOUND;
}

galay_status_t galay_mysql_result_set_value(const galay_mysql_result_set_t* result,
                                            size_t row,
                                            size_t column,
                                            galay_mysql_value_view_t* value)
{
    if (result == nullptr || value == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (row >= result->rows.size() || column >= result->fields.size() ||
        column >= result->rows[row].size()) {
        return GALAY_NOT_FOUND;
    }
    const MysqlResultValue& source = result->rows[row][column];
    value->is_null = source.is_null ? GALAY_TRUE : GALAY_FALSE;
    value->data = source.is_null ? nullptr :
        reinterpret_cast<const unsigned char*>(source.data.data());
    value->data_len = source.is_null ? 0 : source.data.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_affected_rows(const galay_mysql_result_set_t* result,
                                                    uint64_t* affected_rows)
{
    if (result == nullptr || affected_rows == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *affected_rows = result->affected_rows;
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_last_insert_id(const galay_mysql_result_set_t* result,
                                                     uint64_t* last_insert_id)
{
    if (result == nullptr || last_insert_id == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *last_insert_id = result->last_insert_id;
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_status_flags(const galay_mysql_result_set_t* result,
                                                   uint16_t* status_flags)
{
    if (result == nullptr || status_flags == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *status_flags = result->status_flags;
    return GALAY_OK;
}

galay_status_t galay_mysql_result_set_warnings(const galay_mysql_result_set_t* result,
                                               uint16_t* warnings)
{
    if (result == nullptr || warnings == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *warnings = result->warnings;
    return GALAY_OK;
}

galay_status_t galay_mysql_client_create(galay_mysql_client_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mysql_client_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mysql_client_destroy(galay_mysql_client_t* client)
{
    if (client != nullptr && client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
            client->authenticated = false;
        }
    }
    delete client;
}

void galay_mysql_client_close(galay_mysql_client_t* client)
{
    if (client == nullptr) {
        return;
    }
    if (client->socket.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess) {
            client->connected = false;
            client->authenticated = false;
            return;
        }
    }
    client->handshake_packet.clear();
    client->connected = false;
    client->authenticated = false;
}

galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                               galay_bool_t* connected)
{
    if (client == nullptr || connected == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *connected = client->connected ? GALAY_TRUE : GALAY_FALSE;
    return GALAY_OK;
}

galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                          const galay_mysql_config_t* config)
{
    if (client == nullptr || config == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_UNSUPPORTED;
}

C_IOResult galay_mysql_client_connect_async(galay_mysql_client_t* client,
                                            const galay_mysql_config_t* config,
                                            int64_t timeout_ms)
{
    if (client == nullptr || config == nullptr || client->connected ||
        client->socket.socket != nullptr || galay_mysql_config_validate(config) != GALAY_OK) {
        return make_io_result(C_IOResultInvalid);
    }
    C_Host host{};
    if (!copy_host_to_c_host(config->host, config->port, &host)) {
        return make_io_result(C_IOResultInvalid);
    }
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&client->socket, host.type);
    if (created != C_TcpSocketSuccess) {
        return io_result_from_socket_create(created);
    }

    const int64_t effective_timeout =
        timeout_ms < 0 ? static_cast<int64_t>(config->timeout_ms) : timeout_ms;
    C_IOResult connected = galay_kernel_tcp_socket_connect(&client->socket,
                                                           &host,
                                                           effective_timeout);
    if (connected.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && connected.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return connected;
    }

    C_IOResult handshake = read_mysql_packet(&client->socket,
                                             &client->handshake_packet,
                                             effective_timeout);
    if (handshake.code != C_IOResultOk) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
        if (destroyed != C_TcpSocketSuccess && handshake.code == C_IOResultOk) {
            return io_result_from_socket_create(destroyed);
        }
        client->connected = false;
        return handshake;
    }
    client->connected = true;
    client->authenticated = false;
    handshake.ptr = client;
    return handshake;
}

C_IOResult galay_mysql_client_authenticate_async(galay_mysql_client_t* client,
                                                 const galay_mysql_config_t* config,
                                                 int64_t timeout_ms)
{
    if (client == nullptr || config == nullptr || !client->connected ||
        client->socket.socket == nullptr || client->handshake_packet.empty()) {
        return make_io_result(C_IOResultInvalid);
    }
    MysqlHandshakeInfo handshake;
    const galay_status_t parsed = mysql_parse_handshake_info(client->handshake_packet, &handshake);
    if (parsed != GALAY_OK) {
        return io_result_from_status(parsed);
    }
    galay_mysql_buffer_t* response = nullptr;
    const galay_status_t encoded = mysql_encode_handshake_response(config, handshake, &response);
    if (encoded != GALAY_OK) {
        return io_result_from_status(encoded);
    }
    C_IOResult sent = mysql_send_buffer(client, response, timeout_ms);
    galay_mysql_buffer_destroy(response);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    std::vector<unsigned char> auth_result;
    C_IOResult read = read_mysql_packet(&client->socket, &auth_result, timeout_ms);
    if (read.code != C_IOResultOk) {
        return read;
    }
    galay_mysql_packet_view_t view{};
    const galay_status_t packet_status =
        mysql_extract_packet_view_internal(auth_result.data(), auth_result.size(), &view);
    if (packet_status != GALAY_OK || view.payload_len == 0 ||
        !mysql_is_ok_payload(view.payload, view.payload_len)) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    client->authenticated = true;
    read.ptr = client;
    return read;
}

C_IOResult galay_mysql_client_connect_auth_async(galay_mysql_client_t* client,
                                                 const galay_mysql_config_t* config,
                                                 int64_t timeout_ms)
{
    C_IOResult connected = galay_mysql_client_connect_async(client, config, timeout_ms);
    if (connected.code != C_IOResultOk) {
        return connected;
    }
    C_IOResult authed = galay_mysql_client_authenticate_async(client, config, timeout_ms);
    if (authed.code != C_IOResultOk) {
        C_IOResult closed = galay_mysql_client_close_async(client, timeout_ms);
        if (closed.code != C_IOResultOk) {
            return closed;
        }
        return authed;
    }
    return authed;
}

C_IOResult galay_mysql_client_query_async(galay_mysql_client_t* client,
                                          const char* query,
                                          int64_t timeout_ms,
                                          galay_mysql_buffer_t** result_packet)
{
    if (result_packet != nullptr) {
        *result_packet = nullptr;
    }
    if (client == nullptr || query == nullptr || result_packet == nullptr ||
        !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }

    galay_mysql_buffer_t* query_packet = nullptr;
    const galay_status_t encoded = galay_mysql_encode_query_packet(query, 0, &query_packet);
    if (encoded != GALAY_OK) {
        return io_result_from_status(encoded);
    }
    const unsigned char* query_data = nullptr;
    size_t query_len = 0;
    const galay_status_t data_status =
        galay_mysql_buffer_data(query_packet, &query_data, &query_len);
    if (data_status != GALAY_OK) {
        galay_mysql_buffer_destroy(query_packet);
        return io_result_from_status(data_status);
    }

    C_IOResult sent = socket_write_exact(&client->socket, query_data, query_len, timeout_ms);
    galay_mysql_buffer_destroy(query_packet);
    if (sent.code != C_IOResultOk) {
        return sent;
    }

    auto* result = new (std::nothrow) galay_mysql_buffer_t();
    if (result == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    C_IOResult read = read_mysql_packet(&client->socket, &result->data, timeout_ms);
    if (read.code != C_IOResultOk) {
        delete result;
        return read;
    }

    *result_packet = result;
    read.ptr = result;
    return read;
}

C_IOResult galay_mysql_client_query_result_async(galay_mysql_client_t* client,
                                                 const char* query,
                                                 int64_t timeout_ms,
                                                 galay_mysql_result_set_t** result)
{
    return mysql_client_query_result(client, query, timeout_ms, result);
}

C_IOResult galay_mysql_client_begin_transaction_async(galay_mysql_client_t* client,
                                                      int64_t timeout_ms,
                                                      galay_mysql_result_set_t** result)
{
    return mysql_client_query_result(client, "START TRANSACTION", timeout_ms, result);
}

C_IOResult galay_mysql_client_commit_async(galay_mysql_client_t* client,
                                           int64_t timeout_ms,
                                           galay_mysql_result_set_t** result)
{
    return mysql_client_query_result(client, "COMMIT", timeout_ms, result);
}

C_IOResult galay_mysql_client_rollback_async(galay_mysql_client_t* client,
                                             int64_t timeout_ms,
                                             galay_mysql_result_set_t** result)
{
    return mysql_client_query_result(client, "ROLLBACK", timeout_ms, result);
}

C_IOResult galay_mysql_client_stmt_prepare_async(galay_mysql_client_t* client,
                                                 const char* sql,
                                                 int64_t timeout_ms,
                                                 galay_mysql_stmt_t** stmt)
{
    if (stmt != nullptr) {
        *stmt = nullptr;
    }
    if (client == nullptr || sql == nullptr || stmt == nullptr ||
        !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult sent = mysql_send_simple_command(client, kMysqlCommandStmtPrepare, sql, timeout_ms);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    std::vector<unsigned char> packet;
    C_IOResult read = read_mysql_packet(&client->socket, &packet, timeout_ms);
    if (read.code != C_IOResultOk) {
        return read;
    }
    galay_mysql_packet_view_t view{};
    const galay_status_t packet_status =
        mysql_extract_packet_view_internal(packet.data(), packet.size(), &view);
    if (packet_status != GALAY_OK || mysql_is_err_payload(view.payload, view.payload_len)) {
        return io_result_from_status(GALAY_PROTOCOL_ERROR);
    }
    auto* prepared = new (std::nothrow) galay_mysql_stmt_t();
    if (prepared == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    const galay_status_t ok_status =
        mysql_parse_prepare_ok_payload(view.payload, view.payload_len, prepared);
    if (ok_status != GALAY_OK) {
        delete prepared;
        return io_result_from_status(ok_status);
    }
    C_IOResult param_fields =
        mysql_stmt_read_fields(client, timeout_ms, prepared->num_params, &prepared->param_fields);
    if (param_fields.code != C_IOResultOk) {
        delete prepared;
        return param_fields;
    }
    C_IOResult column_fields =
        mysql_stmt_read_fields(client, timeout_ms, prepared->num_columns, &prepared->column_fields);
    if (column_fields.code != C_IOResultOk) {
        delete prepared;
        return column_fields;
    }
    *stmt = prepared;
    read.ptr = prepared;
    return read;
}

void galay_mysql_stmt_destroy(galay_mysql_stmt_t* stmt)
{
    delete stmt;
}

galay_status_t galay_mysql_stmt_id(const galay_mysql_stmt_t* stmt, uint32_t* statement_id)
{
    if (stmt == nullptr || statement_id == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *statement_id = stmt->statement_id;
    return GALAY_OK;
}

galay_status_t galay_mysql_stmt_param_count(const galay_mysql_stmt_t* stmt, size_t* count)
{
    if (stmt == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = stmt->num_params;
    return GALAY_OK;
}

galay_status_t galay_mysql_stmt_column_count(const galay_mysql_stmt_t* stmt, size_t* count)
{
    if (stmt == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = stmt->num_columns;
    return GALAY_OK;
}

C_IOResult galay_mysql_client_stmt_execute_async(galay_mysql_client_t* client,
                                                 const galay_mysql_stmt_t* stmt,
                                                 const galay_mysql_stmt_bind_t* binds,
                                                 size_t bind_count,
                                                 int64_t timeout_ms,
                                                 galay_mysql_result_set_t** result)
{
    if (result != nullptr) {
        *result = nullptr;
    }
    if (client == nullptr || stmt == nullptr || result == nullptr ||
        !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_mysql_buffer_t* packet = nullptr;
    const galay_status_t encoded = mysql_encode_stmt_execute_packet(stmt, binds, bind_count, &packet);
    if (encoded != GALAY_OK) {
        return io_result_from_status(encoded);
    }
    C_IOResult sent = mysql_send_buffer(client, packet, timeout_ms);
    galay_mysql_buffer_destroy(packet);
    if (sent.code != C_IOResultOk) {
        return sent;
    }
    std::vector<unsigned char> packets;
    C_IOResult read = mysql_read_response_packets(client, timeout_ms, &packets);
    if (read.code != C_IOResultOk) {
        return read;
    }
    return mysql_decode_response_packets(packets, result);
}

galay_status_t galay_mysql_pipeline_create(galay_mysql_pipeline_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_mysql_pipeline_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_mysql_pipeline_destroy(galay_mysql_pipeline_t* pipeline)
{
    delete pipeline;
}

galay_status_t galay_mysql_pipeline_append_query(galay_mysql_pipeline_t* pipeline,
                                                 const char* query)
{
    if (pipeline == nullptr || query == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    pipeline->queries.emplace_back(query);
    return GALAY_OK;
}

C_IOResult galay_mysql_client_pipeline_async(galay_mysql_client_t* client,
                                             const galay_mysql_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_mysql_pipeline_result_t** result)
{
    if (result != nullptr) {
        *result = nullptr;
    }
    if (client == nullptr || pipeline == nullptr || result == nullptr ||
        pipeline->queries.empty() || !client->connected || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    for (const std::string& query : pipeline->queries) {
        C_IOResult sent = mysql_send_simple_command(client, kMysqlCommandQuery, query.c_str(), timeout_ms);
        if (sent.code != C_IOResultOk) {
            return sent;
        }
    }
    auto* pipeline_result = new (std::nothrow) galay_mysql_pipeline_result_t();
    if (pipeline_result == nullptr) {
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    pipeline_result->results.reserve(pipeline->queries.size());
    for (size_t i = 0; i < pipeline->queries.size(); ++i) {
        std::vector<unsigned char> packets;
        C_IOResult read = mysql_read_response_packets(client, timeout_ms, &packets);
        if (read.code != C_IOResultOk) {
            delete pipeline_result;
            return read;
        }
        galay_mysql_result_set_t decoded;
        const galay_status_t status =
            mysql_decode_result_packets(packets.data(), packets.size(), &decoded);
        if (status != GALAY_OK) {
            delete pipeline_result;
            return io_result_from_status(status);
        }
        pipeline_result->results.push_back(std::move(decoded));
    }
    *result = pipeline_result;
    C_IOResult io = make_io_result(C_IOResultOk);
    io.value = static_cast<int64_t>(pipeline_result->results.size());
    io.ptr = pipeline_result;
    return io;
}

void galay_mysql_pipeline_result_destroy(galay_mysql_pipeline_result_t* result)
{
    delete result;
}

galay_status_t galay_mysql_pipeline_result_count(const galay_mysql_pipeline_result_t* result,
                                                 size_t* count)
{
    if (result == nullptr || count == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *count = result->results.size();
    return GALAY_OK;
}

galay_status_t galay_mysql_pipeline_result_at(const galay_mysql_pipeline_result_t* result,
                                              size_t index,
                                              const galay_mysql_result_set_t** item)
{
    if (result == nullptr || item == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (index >= result->results.size()) {
        return GALAY_NOT_FOUND;
    }
    *item = &result->results[index];
    return GALAY_OK;
}

galay_status_t galay_mysql_pool_create(const galay_mysql_config_t* config,
                                       size_t max_connections,
                                       galay_mysql_pool_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (config == nullptr || out == nullptr || max_connections == 0 ||
        galay_mysql_config_validate(config) != GALAY_OK) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* pool = new (std::nothrow) galay_mysql_pool_t();
    if (pool == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    pool->config = *config;
    pool->max_connections = max_connections;
    *out = pool;
    return GALAY_OK;
}

void galay_mysql_pool_destroy(galay_mysql_pool_t* pool)
{
    if (pool == nullptr) {
        return;
    }
    for (galay_mysql_client_t* client : pool->idle) {
        galay_mysql_client_destroy(client);
    }
    delete pool;
}

C_IOResult galay_mysql_pool_acquire_async(galay_mysql_pool_t* pool,
                                          int64_t timeout_ms,
                                          galay_mysql_pool_lease_t** lease)
{
    if (lease != nullptr) {
        *lease = nullptr;
    }
    if (pool == nullptr || lease == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_mysql_client_t* client = nullptr;
    if (!pool->idle.empty()) {
        client = pool->idle.back();
        pool->idle.pop_back();
    } else {
        if (pool->total_connections >= pool->max_connections) {
            return io_result_from_status(GALAY_UNSUPPORTED);
        }
        const galay_status_t created = galay_mysql_client_create(&client);
        if (created != GALAY_OK) {
            return io_result_from_status(created);
        }
        ++pool->total_connections;
        C_IOResult connected = galay_mysql_client_connect_auth_async(client, &pool->config, timeout_ms);
        if (connected.code != C_IOResultOk) {
            galay_mysql_client_destroy(client);
            --pool->total_connections;
            return connected;
        }
    }
    auto* acquired = new (std::nothrow) galay_mysql_pool_lease_t();
    if (acquired == nullptr) {
        pool->idle.push_back(client);
        return io_result_from_status(GALAY_OUT_OF_MEMORY);
    }
    acquired->pool = pool;
    acquired->client = client;
    *lease = acquired;
    C_IOResult io = make_io_result(C_IOResultOk);
    io.ptr = acquired;
    return io;
}

galay_status_t galay_mysql_pool_lease_client(galay_mysql_pool_lease_t* lease,
                                             galay_mysql_client_t** client)
{
    if (lease == nullptr || client == nullptr || lease->client == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *client = lease->client;
    return GALAY_OK;
}

galay_status_t galay_mysql_pool_lease_release(galay_mysql_pool_lease_t* lease)
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

C_IOResult galay_mysql_client_close_async(galay_mysql_client_t* client, int64_t timeout_ms)
{
    if (client == nullptr || client->socket.socket == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    C_IOResult close_result = galay_kernel_tcp_socket_close(&client->socket, timeout_ms);
    const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&client->socket);
    client->connected = false;
    client->authenticated = false;
    client->handshake_packet.clear();
    if (close_result.code == C_IOResultOk && destroyed != C_TcpSocketSuccess) {
        return io_result_from_socket_create(destroyed);
    }
    return close_result;
}

}
