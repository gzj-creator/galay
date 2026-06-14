#include "mysql_protocol.h"
#include <cstring>
#include <algorithm>
#include <concepts>

namespace galay::mysql::protocol
{

// ======================== 辅助函数实现 ========================

uint16_t readUint16(const char* data)
{
    return static_cast<uint8_t>(data[0]) |
           (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
}

uint32_t readUint24(const char* data)
{
    return static_cast<uint8_t>(data[0]) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16);
}

uint32_t readUint32(const char* data)
{
    return static_cast<uint8_t>(data[0]) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
}

uint64_t readUint64(const char* data)
{
    uint64_t lo = readUint32(data);
    uint64_t hi = readUint32(data + 4);
    return lo | (hi << 32);
}

void writeUint16(std::string& buf, uint16_t val)
{
    buf.push_back(static_cast<char>(val & 0xFF));
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
}

void writeUint24(std::string& buf, uint32_t val)
{
    buf.push_back(static_cast<char>(val & 0xFF));
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<char>((val >> 16) & 0xFF));
}

void writeUint32(std::string& buf, uint32_t val)
{
    buf.push_back(static_cast<char>(val & 0xFF));
    buf.push_back(static_cast<char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<char>((val >> 16) & 0xFF));
    buf.push_back(static_cast<char>((val >> 24) & 0xFF));
}

void writeUint64(std::string& buf, uint64_t val)
{
    writeUint32(buf, static_cast<uint32_t>(val & 0xFFFFFFFF));
    writeUint32(buf, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
}

void writeLenEncInt(std::string& buf, uint64_t val)
{
    if (val < 251) {
        buf.push_back(static_cast<char>(val));
    } else if (val < 0x10000) {
        buf.push_back(static_cast<char>(0xFC));
        writeUint16(buf, static_cast<uint16_t>(val));
    } else if (val < 0x1000000) {
        buf.push_back(static_cast<char>(0xFD));
        writeUint24(buf, static_cast<uint32_t>(val));
    } else {
        buf.push_back(static_cast<char>(0xFE));
        writeUint64(buf, val);
    }
}

void writeLenEncString(std::string& buf, std::string_view str)
{
    writeLenEncInt(buf, str.size());
    buf.append(str.data(), str.size());
}

std::expected<uint64_t, ParseError> readLenEncInt(const char* data, size_t len, size_t& consumed)
{
    if (len < 1) return std::unexpected(ParseError::Incomplete);

    uint8_t first = static_cast<uint8_t>(data[0]);
    if (first < 0xFB) {
        consumed = 1;
        return first;
    } else if (first == 0xFB) {
        // NULL indicator in result set rows
        consumed = 1;
        return static_cast<uint64_t>(0xFB);
    } else if (first == 0xFC) {
        if (len < 3) return std::unexpected(ParseError::Incomplete);
        consumed = 3;
        return readUint16(data + 1);
    } else if (first == 0xFD) {
        if (len < 4) return std::unexpected(ParseError::Incomplete);
        consumed = 4;
        return readUint24(data + 1);
    } else if (first == 0xFE) {
        if (len < 9) return std::unexpected(ParseError::Incomplete);
        consumed = 9;
        return readUint64(data + 1);
    }
    return std::unexpected(ParseError::InvalidFormat);
}

std::expected<std::string, ParseError> readLenEncString(const char* data, size_t len, size_t& consumed)
{
    size_t int_consumed = 0;
    auto int_result = readLenEncInt(data, len, int_consumed);
    if (!int_result) return std::unexpected(int_result.error());

    uint64_t str_len = int_result.value();
    if (len < int_consumed + str_len) return std::unexpected(ParseError::Incomplete);

    consumed = int_consumed + str_len;
    return std::string(data + int_consumed, str_len);
}

std::expected<std::string, ParseError> readNullTermString(const char* data, size_t len, size_t& consumed)
{
    const char* end = static_cast<const char*>(memchr(data, '\0', len));
    if (!end) return std::unexpected(ParseError::Incomplete);

    size_t str_len = end - data;
    consumed = str_len + 1; // 包含null终止符
    return std::string(data, str_len);
}

// ======================== MysqlParser 实现 ========================

std::expected<PacketHeader, ParseError> MysqlParser::parseHeader(const char* data, size_t len)
{
    if (len < MYSQL_PACKET_HEADER_SIZE) {
        return std::unexpected(ParseError::Incomplete);
    }

    PacketHeader header;
    header.length = readUint24(data);
    header.sequence_id = static_cast<uint8_t>(data[3]);
    return header;
}

std::expected<MysqlParser::PacketView, ParseError>
MysqlParser::extractPacket(const char* data, size_t len, size_t& consumed)
{
    auto header_result = parseHeader(data, len);
    if (!header_result) return std::unexpected(header_result.error());

    auto& header = header_result.value();
    size_t total = MYSQL_PACKET_HEADER_SIZE + header.length;
    if (len < total) return std::unexpected(ParseError::Incomplete);

    consumed = total;
    return PacketView{
        data + MYSQL_PACKET_HEADER_SIZE,
        header.length,
        header.sequence_id
    };
}

std::expected<HandshakeV10, ParseError>
MysqlParser::parseHandshake(const char* data, size_t len)
{
    if (len < 1) return std::unexpected(ParseError::Incomplete);

    HandshakeV10 hs;
    size_t pos = 0;

    // protocol_version
    hs.protocol_version = static_cast<uint8_t>(data[pos++]);
    if (hs.protocol_version != 10) {
        return std::unexpected(ParseError::InvalidFormat);
    }

    // server_version (null-terminated)
    size_t consumed = 0;
    auto sv = readNullTermString(data + pos, len - pos, consumed);
    if (!sv) return std::unexpected(sv.error());
    hs.server_version = std::move(sv.value());
    pos += consumed;

    // connection_id (4 bytes)
    if (pos + 4 > len) return std::unexpected(ParseError::Incomplete);
    hs.connection_id = readUint32(data + pos);
    pos += 4;

    // auth_plugin_data_part_1 (8 bytes)
    if (pos + 8 > len) return std::unexpected(ParseError::Incomplete);
    hs.auth_plugin_data.assign(data + pos, 8);
    pos += 8;

    // filler (1 byte, 0x00)
    if (pos + 1 > len) return std::unexpected(ParseError::Incomplete);
    pos += 1;

    // capability_flags_lower (2 bytes)
    if (pos + 2 > len) return std::unexpected(ParseError::Incomplete);
    hs.capability_flags = readUint16(data + pos);
    pos += 2;

    if (pos >= len) {
        // 最小握手包到此结束
        return hs;
    }

    // character_set (1 byte)
    hs.character_set = static_cast<uint8_t>(data[pos++]);

    // status_flags (2 bytes)
    if (pos + 2 > len) return std::unexpected(ParseError::Incomplete);
    hs.status_flags = readUint16(data + pos);
    pos += 2;

    // capability_flags_upper (2 bytes)
    if (pos + 2 > len) return std::unexpected(ParseError::Incomplete);
    hs.capability_flags |= (static_cast<uint32_t>(readUint16(data + pos)) << 16);
    pos += 2;

    // auth_plugin_data_len or 0x00 (1 byte)
    if (pos + 1 > len) return std::unexpected(ParseError::Incomplete);
    uint8_t auth_plugin_data_len = static_cast<uint8_t>(data[pos++]);

    // reserved (10 bytes, all 0x00)
    if (pos + 10 > len) return std::unexpected(ParseError::Incomplete);
    pos += 10;

    // auth_plugin_data_part_2 (if CLIENT_SECURE_CONNECTION)
    if (hs.capability_flags & CLIENT_SECURE_CONNECTION) {
        size_t part2_len = std::max(13, static_cast<int>(auth_plugin_data_len) - 8);
        if (pos + part2_len > len) return std::unexpected(ParseError::Incomplete);
        // 追加到auth_plugin_data，去掉末尾的null
        size_t actual_len = part2_len;
        if (actual_len > 0 && data[pos + actual_len - 1] == '\0') {
            actual_len--;
        }
        hs.auth_plugin_data.append(data + pos, actual_len);
        pos += part2_len;
    }

    // auth_plugin_name (if CLIENT_PLUGIN_AUTH)
    if (hs.capability_flags & CLIENT_PLUGIN_AUTH) {
        auto apn = readNullTermString(data + pos, len - pos, consumed);
        if (apn) {
            hs.auth_plugin_name = std::move(apn.value());
            pos += consumed;
        }
    }

    return hs;
}

ResponseType MysqlParser::identifyResponse(uint8_t first_byte, uint32_t payload_len)
{
    if (first_byte == 0x00 && payload_len >= 7) {
        return ResponseType::OK;
    } else if (first_byte == 0xFF) {
        return ResponseType::ERR;
    } else if (first_byte == 0xFE && payload_len < 9) {
        return ResponseType::EOF_PKT;
    } else if (first_byte == 0xFB) {
        return ResponseType::LOCAL_INFILE;
    }
    // 否则是结果集的列数
    return ResponseType::OK; // 将作为len_enc_int处理
}

std::expected<OkPacket, ParseError>
MysqlParser::parseOk(const char* data, size_t len, uint32_t capabilities)
{
    if (len < 1) return std::unexpected(ParseError::Incomplete);

    OkPacket ok;
    size_t pos = 1; // 跳过0x00标识字节
    size_t consumed = 0;

    // affected_rows (len_enc_int)
    auto ar = readLenEncInt(data + pos, len - pos, consumed);
    if (!ar) return std::unexpected(ar.error());
    ok.affected_rows = ar.value();
    pos += consumed;

    // last_insert_id (len_enc_int)
    auto li = readLenEncInt(data + pos, len - pos, consumed);
    if (!li) return std::unexpected(li.error());
    ok.last_insert_id = li.value();
    pos += consumed;

    // status_flags + warnings (if CLIENT_PROTOCOL_41)
    if (capabilities & CLIENT_PROTOCOL_41) {
        if (pos + 4 > len) return std::unexpected(ParseError::Incomplete);
        ok.status_flags = readUint16(data + pos);
        pos += 2;
        ok.warnings = readUint16(data + pos);
        pos += 2;
    }

    // info (remaining bytes)
    if (pos < len) {
        ok.info.assign(data + pos, len - pos);
    }

    return ok;
}

std::expected<ErrPacket, ParseError>
MysqlParser::parseErr(const char* data, size_t len, uint32_t capabilities)
{
    if (len < 3) return std::unexpected(ParseError::Incomplete);

    ErrPacket err;
    size_t pos = 1; // 跳过0xFF标识字节

    // error_code (2 bytes)
    err.error_code = readUint16(data + pos);
    pos += 2;

    // sql_state_marker + sql_state (if CLIENT_PROTOCOL_41)
    if (capabilities & CLIENT_PROTOCOL_41) {
        if (pos + 6 > len) return std::unexpected(ParseError::Incomplete);
        pos += 1; // '#' marker
        err.sql_state.assign(data + pos, 5);
        pos += 5;
    }

    // error_message (remaining bytes)
    if (pos < len) {
        err.error_message.assign(data + pos, len - pos);
    }

    return err;
}

std::expected<EofPacket, ParseError>
MysqlParser::parseEof(const char* data, size_t len)
{
    if (len < 1) return std::unexpected(ParseError::Incomplete);

    EofPacket eof;
    size_t pos = 1; // 跳过0xFE标识字节

    if (pos + 4 <= len) {
        eof.warnings = readUint16(data + pos);
        pos += 2;
        eof.status_flags = readUint16(data + pos);
        pos += 2;
    }

    return eof;
}

std::expected<ColumnDefinitionPacket, ParseError>
MysqlParser::parseColumnDefinition(const char* data, size_t len)
{
    ColumnDefinitionPacket col;
    size_t pos = 0;
    size_t consumed = 0;

    // catalog (len_enc_string)
    auto cat = readLenEncString(data + pos, len - pos, consumed);
    if (!cat) return std::unexpected(cat.error());
    col.catalog = std::move(cat.value());
    pos += consumed;

    // schema
    auto sch = readLenEncString(data + pos, len - pos, consumed);
    if (!sch) return std::unexpected(sch.error());
    col.schema = std::move(sch.value());
    pos += consumed;

    // table
    auto tbl = readLenEncString(data + pos, len - pos, consumed);
    if (!tbl) return std::unexpected(tbl.error());
    col.table = std::move(tbl.value());
    pos += consumed;

    // org_table
    auto otbl = readLenEncString(data + pos, len - pos, consumed);
    if (!otbl) return std::unexpected(otbl.error());
    col.org_table = std::move(otbl.value());
    pos += consumed;

    // name
    auto nm = readLenEncString(data + pos, len - pos, consumed);
    if (!nm) return std::unexpected(nm.error());
    col.name = std::move(nm.value());
    pos += consumed;

    // org_name
    auto onm = readLenEncString(data + pos, len - pos, consumed);
    if (!onm) return std::unexpected(onm.error());
    col.org_name = std::move(onm.value());
    pos += consumed;

    // fixed-length fields (0x0c length prefix)
    if (pos + 1 > len) return std::unexpected(ParseError::Incomplete);
    pos += 1; // skip length of fixed-length fields (0x0c)

    if (pos + 12 > len) return std::unexpected(ParseError::Incomplete);
    col.character_set = readUint16(data + pos); pos += 2;
    col.column_length = readUint32(data + pos); pos += 4;
    col.column_type = static_cast<uint8_t>(data[pos]); pos += 1;
    col.flags = readUint16(data + pos); pos += 2;
    col.decimals = static_cast<uint8_t>(data[pos]); pos += 1;
    pos += 2; // filler

    return col;
}

std::expected<std::vector<std::optional<std::string>>, ParseError>
MysqlParser::parseTextRow(const char* data, size_t len, size_t column_count)
{
    std::vector<std::optional<std::string>> row;
    row.reserve(column_count);
    size_t pos = 0;

    for (size_t i = 0; i < column_count; ++i) {
        if (pos >= len) return std::unexpected(ParseError::Incomplete);

        if (static_cast<uint8_t>(data[pos]) == 0xFB) {
            // NULL
            row.push_back(std::nullopt);
            pos += 1;
        } else {
            size_t consumed = 0;
            auto val = readLenEncString(data + pos, len - pos, consumed);
            if (!val) return std::unexpected(val.error());
            row.push_back(std::move(val.value()));
            pos += consumed;
        }
    }

    return row;
}

std::expected<StmtPrepareOkPacket, ParseError>
MysqlParser::parseStmtPrepareOk(const char* data, size_t len)
{
    if (len < 12) return std::unexpected(ParseError::Incomplete);

    StmtPrepareOkPacket pkt;
    size_t pos = 1; // 跳过0x00标识字节

    pkt.statement_id = readUint32(data + pos); pos += 4;
    pkt.num_columns = readUint16(data + pos); pos += 2;
    pkt.num_params = readUint16(data + pos); pos += 2;
    pos += 1; // filler
    pkt.warning_count = readUint16(data + pos); pos += 2;

    return pkt;
}

// ======================== MysqlEncoder 实现 ========================

namespace {

template<typename ParamSpan>
concept StmtExecuteParamSpan = requires(const ParamSpan& params, size_t i) {
    { params.empty() } -> std::convertible_to<bool>;
    { params.size() } -> std::convertible_to<size_t>;
    { params[i].has_value() } -> std::convertible_to<bool>;
    { *params[i] } -> std::convertible_to<std::string_view>;
};

template<StmtExecuteParamSpan ParamSpan>
std::string encodeStmtExecuteImpl(uint32_t stmt_id,
                                  ParamSpan params,
                                  std::span<const uint8_t> param_types,
                                  uint8_t sequence_id)
{
    auto len_enc_size = [](size_t n) -> size_t {
        if (n < 251) return 1;
        if (n < (1ULL << 16)) return 3;
        if (n < (1ULL << 24)) return 4;
        return 9;
    };

    size_t payload_reserve = 10; // cmd(1) + stmt_id(4) + flags(1) + iteration_count(4)
    if (!params.empty()) {
        const size_t null_bitmap_len = (params.size() + 7) / 8;
        payload_reserve += null_bitmap_len + 1 + params.size() * 2;
        for (const auto& param : params) {
            if (param.has_value()) {
                const std::string_view value = *param;
                payload_reserve += len_enc_size(value.size()) + value.size();
            }
        }
    }

    std::string payload;
    payload.reserve(payload_reserve);
    payload.push_back(static_cast<char>(CommandType::COM_STMT_EXECUTE));

    // statement_id (4 bytes)
    writeUint32(payload, stmt_id);

    // flags (1 byte) - CURSOR_TYPE_NO_CURSOR
    payload.push_back(0x00);

    // iteration_count (4 bytes) - always 1
    writeUint32(payload, 1);

    if (!params.empty()) {
        // NULL bitmap
        size_t null_bitmap_len = (params.size() + 7) / 8;
        const size_t null_bitmap_pos = payload.size();
        payload.append(null_bitmap_len, '\0');
        for (size_t i = 0; i < params.size(); ++i) {
            if (!params[i].has_value()) {
                payload[null_bitmap_pos + (i / 8)] |= static_cast<char>(1u << (i % 8));
            }
        }

        // new_params_bound_flag (1 byte)
        payload.push_back(0x01);

        // parameter types (2 bytes each)
        for (size_t i = 0; i < params.size(); ++i) {
            if (i < param_types.size()) {
                payload.push_back(static_cast<char>(param_types[i]));
            } else {
                payload.push_back(static_cast<char>(MysqlFieldType::VAR_STRING));
            }
            payload.push_back(0x00); // unsigned flag
        }

        // parameter values
        for (const auto& param : params) {
            if (param.has_value()) {
                writeLenEncString(payload, std::string_view(*param));
            }
        }
    }

    std::string packet;
    packet.reserve(MYSQL_PACKET_HEADER_SIZE + payload.size());
    writeUint24(packet, static_cast<uint32_t>(payload.size()));
    packet.push_back(static_cast<char>(sequence_id));
    packet.append(payload.data(), payload.size());
    return packet;
}

} // namespace

std::string MysqlEncoder::wrapPacket(std::string_view payload, uint8_t sequence_id)
{
    std::string packet;
    packet.reserve(MYSQL_PACKET_HEADER_SIZE + payload.size());
    writeUint24(packet, static_cast<uint32_t>(payload.size()));
    packet.push_back(static_cast<char>(sequence_id));
    packet.append(payload.data(), payload.size());
    return packet;
}

std::string MysqlEncoder::encodeSimpleCommand(CommandType cmd, std::string_view payload, uint8_t sequence_id)
{
    const uint32_t payload_len = 1U + static_cast<uint32_t>(payload.size());
    std::string packet;
    packet.reserve(MYSQL_PACKET_HEADER_SIZE + payload_len);
    writeUint24(packet, payload_len);
    packet.push_back(static_cast<char>(sequence_id));
    packet.push_back(static_cast<char>(cmd));
    packet.append(payload.data(), payload.size());
    return packet;
}

std::string MysqlEncoder::encodeHandshakeResponse(const HandshakeResponse41& resp, uint8_t sequence_id)
{
    std::string payload;
    payload.reserve(128);

    // capability_flags (4 bytes)
    writeUint32(payload, resp.capability_flags);

    // max_packet_size (4 bytes)
    writeUint32(payload, resp.max_packet_size);

    // character_set (1 byte)
    payload.push_back(static_cast<char>(resp.character_set));

    // reserved (23 bytes, all 0x00)
    payload.append(23, '\0');

    // username (null-terminated)
    payload.append(resp.username);
    payload.push_back('\0');

    // auth_response
    if (resp.capability_flags & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        writeLenEncString(payload, resp.auth_response);
    } else if (resp.capability_flags & CLIENT_SECURE_CONNECTION) {
        payload.push_back(static_cast<char>(resp.auth_response.size()));
        payload.append(resp.auth_response);
    } else {
        payload.append(resp.auth_response);
        payload.push_back('\0');
    }

    // database (if CLIENT_CONNECT_WITH_DB)
    if (resp.capability_flags & CLIENT_CONNECT_WITH_DB) {
        payload.append(resp.database);
        payload.push_back('\0');
    }

    // auth_plugin_name (if CLIENT_PLUGIN_AUTH)
    if (resp.capability_flags & CLIENT_PLUGIN_AUTH) {
        payload.append(resp.auth_plugin_name);
        payload.push_back('\0');
    }

    return wrapPacket(payload, sequence_id);
}

std::string MysqlEncoder::encodeQuery(std::string_view sql, uint8_t sequence_id)
{
    return encodeSimpleCommand(CommandType::COM_QUERY, sql, sequence_id);
}

std::string MysqlEncoder::encodeStmtPrepare(std::string_view sql, uint8_t sequence_id)
{
    return encodeSimpleCommand(CommandType::COM_STMT_PREPARE, sql, sequence_id);
}

std::string MysqlEncoder::encodeStmtExecute(uint32_t stmt_id,
                                             std::span<const std::optional<std::string>> params,
                                             std::span<const uint8_t> param_types,
                                             uint8_t sequence_id)
{
    return encodeStmtExecuteImpl(stmt_id, params, param_types, sequence_id);
}

std::string MysqlEncoder::encodeStmtExecute(uint32_t stmt_id,
                                             std::span<const std::optional<std::string_view>> params,
                                             std::span<const uint8_t> param_types,
                                             uint8_t sequence_id)
{
    return encodeStmtExecuteImpl(stmt_id, params, param_types, sequence_id);
}

std::string MysqlEncoder::encodeStmtClose(uint32_t stmt_id, uint8_t sequence_id)
{
    std::string payload;
    payload.push_back(static_cast<char>(CommandType::COM_STMT_CLOSE));
    writeUint32(payload, stmt_id);
    return wrapPacket(payload, sequence_id);
}

std::string MysqlEncoder::encodeQuit(uint8_t sequence_id)
{
    return encodeSimpleCommand(CommandType::COM_QUIT, "", sequence_id);
}

std::string MysqlEncoder::encodePing(uint8_t sequence_id)
{
    return encodeSimpleCommand(CommandType::COM_PING, "", sequence_id);
}

std::string MysqlEncoder::encodeInitDb(std::string_view database, uint8_t sequence_id)
{
    return encodeSimpleCommand(CommandType::COM_INIT_DB, database, sequence_id);
}

std::string MysqlEncoder::encodeResetConnection(uint8_t sequence_id)
{
    return encodeSimpleCommand(CommandType::COM_RESET_CONNECTION, "", sequence_id);
}

} // namespace galay::mysql::protocol
