#include "postgres_protocol.h"

#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <limits>
#include <utility>

namespace galay::postgres::protocol
{

namespace
{

bool has_embedded_null(std::string_view value)
{
    return value.find('\0') != std::string_view::npos;
}

bool valid_message_payload_size(size_t size)
{
    constexpr size_t kMaxPayload =
        static_cast<size_t>(std::numeric_limits<int32_t>::max()) - kLengthFieldSize;
    return size <= kMaxPayload;
}

int16_t read_signed_int16(const char* data) noexcept
{
    return std::bit_cast<int16_t>(read_int16(data));
}

int32_t read_signed_int32(const char* data) noexcept
{
    return std::bit_cast<int32_t>(read_int32(data));
}

std::expected<void, ParseError> parse_empty_payload(size_t length)
{
    if (length != 0) {
        return std::unexpected(ParseError::InvalidLength);
    }
    return {};
}

template <typename ParameterSpan>
std::expected<std::string, ParseError> build_bind_payload(std::string_view portal_name,
                                                        std::string_view statement_name,
                                                        ParameterSpan parameters)
{
    if (has_embedded_null(portal_name) || has_embedded_null(statement_name) ||
        parameters.size() > static_cast<size_t>(std::numeric_limits<int16_t>::max())) {
        return std::unexpected(ParseError::InvalidLength);
    }

    std::string payload;
    write_c_string(payload, portal_name);
    write_c_string(payload, statement_name);
    write_int16(payload, 0); // Zero format codes selects the default text format for all parameters.
    write_int16(payload, static_cast<uint16_t>(parameters.size()));
    for (const auto& parameter : parameters) {
        if (!parameter.has_value()) {
            write_int32(payload, std::numeric_limits<uint32_t>::max());
            continue;
        }
        const std::string_view value = *parameter;
        if (value.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return std::unexpected(ParseError::InvalidLength);
        }
        write_int32(payload, static_cast<uint32_t>(value.size()));
        payload.append(value.data(), value.size());
    }
    write_int16(payload, 0); // Request text results for every column.
    return payload;
}

} // namespace

uint16_t read_int16(const char* data) noexcept
{
    return (static_cast<uint16_t>(static_cast<uint8_t>(data[0])) << 8) |
           static_cast<uint8_t>(data[1]);
}

uint32_t read_int32(const char* data) noexcept
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 8) |
           static_cast<uint8_t>(data[3]);
}

void write_int16(std::string& output, uint16_t value)
{
    output.push_back(static_cast<char>((value >> 8) & 0xff));
    output.push_back(static_cast<char>(value & 0xff));
}

void write_int32(std::string& output, uint32_t value)
{
    output.push_back(static_cast<char>((value >> 24) & 0xff));
    output.push_back(static_cast<char>((value >> 16) & 0xff));
    output.push_back(static_cast<char>((value >> 8) & 0xff));
    output.push_back(static_cast<char>(value & 0xff));
}

std::expected<std::string, ParseError> read_c_string(const char* data,
                                                   size_t length,
                                                   size_t& consumed)
{
    if (length == 0) {
        return std::unexpected(ParseError::Incomplete);
    }
    const void* terminator = std::memchr(data, '\0', length);
    if (terminator == nullptr) {
        return std::unexpected(ParseError::Incomplete);
    }
    const auto* end = static_cast<const char*>(terminator);
    const size_t string_length = static_cast<size_t>(end - data);
    consumed = string_length + 1;
    return std::string(data, string_length);
}

void write_c_string(std::string& output, std::string_view value)
{
    output.append(value.data(), value.size());
    output.push_back('\0');
}

std::expected<MessageHeader, ParseError>
PostgresParser::parse_header(const char* data, size_t length) const
{
    if (length < kMessageHeaderSize) {
        return std::unexpected(ParseError::Incomplete);
    }
    const uint32_t message_length = read_int32(data + 1);
    if (message_length < kLengthFieldSize ||
        message_length > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return std::unexpected(ParseError::InvalidLength);
    }
    return MessageHeader{.type = data[0], .length = message_length};
}

std::expected<MessageView, ParseError>
PostgresParser::extract_message(const char* data, size_t length) const
{
    auto header = parse_header(data, length);
    if (!header) {
        return std::unexpected(header.error());
    }
    const size_t total_length = 1 + static_cast<size_t>(header->length);
    if (length < total_length) {
        return std::unexpected(ParseError::Incomplete);
    }
    return MessageView{
        .payload = data + kMessageHeaderSize,
        .consumed = total_length,
        .payload_len = header->length - static_cast<uint32_t>(kLengthFieldSize),
        .type = header->type,
    };
}

std::expected<AuthenticationRequest, ParseError>
PostgresParser::parse_authentication_request(const char* data, size_t length) const
{
    if (length < 4) {
        return std::unexpected(ParseError::Incomplete);
    }

    const uint32_t raw_kind = read_int32(data);
    AuthenticationRequest request;
    switch (raw_kind) {
    case 0: request.kind = AuthRequestKind::Ok; break;
    case 2: request.kind = AuthRequestKind::KerberosV5; break;
    case 3: request.kind = AuthRequestKind::CleartextPassword; break;
    case 5: request.kind = AuthRequestKind::Md5Password; break;
    case 7: request.kind = AuthRequestKind::Gss; break;
    case 8: request.kind = AuthRequestKind::GssContinue; break;
    case 9: request.kind = AuthRequestKind::Sspi; break;
    case 10: request.kind = AuthRequestKind::Sasl; break;
    case 11: request.kind = AuthRequestKind::SaslContinue; break;
    case 12: request.kind = AuthRequestKind::SaslFinal; break;
    default: return std::unexpected(ParseError::InvalidType);
    }

    if (request.kind == AuthRequestKind::Sasl) {
        size_t position = 4;
        while (position < length) {
            size_t consumed = 0;
            auto mechanism = read_c_string(data + position, length - position, consumed);
            if (!mechanism) {
                return std::unexpected(mechanism.error());
            }
            position += consumed;
            if (mechanism->empty()) {
                if (request.mechanisms.empty() || position != length) {
                    return std::unexpected(ParseError::InvalidFormat);
                }
                return request;
            }
            request.mechanisms.push_back(std::move(*mechanism));
        }
        return std::unexpected(ParseError::Incomplete);
    }

    if (request.kind == AuthRequestKind::SaslContinue ||
        request.kind == AuthRequestKind::SaslFinal ||
        request.kind == AuthRequestKind::GssContinue) {
        request.data.assign(data + 4, length - 4);
        return request;
    }

    const size_t expected_length = request.kind == AuthRequestKind::Md5Password ? 8 : 4;
    if (length != expected_length) {
        return std::unexpected(ParseError::InvalidLength);
    }
    if (request.kind == AuthRequestKind::Md5Password) {
        request.data.assign(data + 4, 4);
    }
    return request;
}

std::expected<ErrorFields, ParseError>
PostgresParser::parse_error_response(const char* data, size_t length) const
{
    if (length == 0) {
        return std::unexpected(ParseError::Incomplete);
    }

    ErrorFields fields;
    std::array<bool, 256> seen{};
    bool has_nonlocalized_severity = false;
    size_t position = 0;
    while (position < length) {
        const auto tag = static_cast<uint8_t>(data[position++]);
        if (tag == 0) {
            if (position != length) {
                return std::unexpected(ParseError::InvalidFormat);
            }
            return fields;
        }
        if (seen[tag]) {
            return std::unexpected(ParseError::InvalidFormat);
        }
        seen[tag] = true;

        size_t consumed = 0;
        auto value = read_c_string(data + position, length - position, consumed);
        if (!value) {
            return std::unexpected(value.error());
        }
        position += consumed;

        switch (static_cast<char>(tag)) {
        case 'S':
            if (!has_nonlocalized_severity) fields.severity = std::move(*value);
            break;
        case 'V': fields.severity = std::move(*value); has_nonlocalized_severity = true; break;
        case 'C': fields.sql_state = std::move(*value); break;
        case 'M': fields.message = std::move(*value); break;
        case 'D': fields.detail = std::move(*value); break;
        case 'H': fields.hint = std::move(*value); break;
        case 'P': fields.position = std::move(*value); break;
        case 'p': fields.internal_position = std::move(*value); break;
        case 'q': fields.internal_query = std::move(*value); break;
        case 'W': fields.where = std::move(*value); break;
        case 's': fields.schema_name = std::move(*value); break;
        case 't': fields.table_name = std::move(*value); break;
        case 'c': fields.column_name = std::move(*value); break;
        case 'd': fields.data_type_name = std::move(*value); break;
        case 'n': fields.constraint_name = std::move(*value); break;
        case 'F': fields.file = std::move(*value); break;
        case 'L': fields.line = std::move(*value); break;
        case 'R': fields.routine = std::move(*value); break;
        default: break;
        }
    }
    return std::unexpected(ParseError::Incomplete);
}

std::expected<std::vector<RowDescriptionField>, ParseError>
PostgresParser::parse_row_description(const char* data, size_t length) const
{
    if (length < 2) {
        return std::unexpected(ParseError::Incomplete);
    }
    const int16_t signed_count = read_signed_int16(data);
    if (signed_count < 0) {
        return std::unexpected(ParseError::InvalidLength);
    }
    const size_t count = static_cast<size_t>(signed_count);
    if (count > (length - 2) / 19) {
        return std::unexpected(ParseError::Incomplete);
    }

    std::vector<RowDescriptionField> fields;
    fields.reserve(count);
    size_t position = 2;
    for (size_t index = 0; index < count; ++index) {
        size_t consumed = 0;
        auto name = read_c_string(data + position, length - position, consumed);
        if (!name) {
            return std::unexpected(name.error());
        }
        position += consumed;
        if (length - position < 18) {
            return std::unexpected(ParseError::Incomplete);
        }

        RowDescriptionField field;
        field.name = std::move(*name);
        field.table_oid = read_int32(data + position); position += 4;
        field.column_index = read_signed_int16(data + position); position += 2;
        field.type_oid = read_int32(data + position); position += 4;
        field.type_size = read_signed_int16(data + position); position += 2;
        field.type_modifier = read_signed_int32(data + position); position += 4;
        field.format = read_signed_int16(data + position); position += 2;
        if (field.format != 0 && field.format != 1) {
            return std::unexpected(ParseError::InvalidFormat);
        }
        fields.push_back(std::move(field));
    }
    if (position != length) {
        return std::unexpected(ParseError::InvalidLength);
    }
    return fields;
}

std::expected<std::vector<std::optional<std::string_view>>, ParseError>
PostgresParser::parse_data_row_view(const char* data, size_t length) const
{
    if (length < 2) {
        return std::unexpected(ParseError::Incomplete);
    }
    const int16_t signed_count = read_signed_int16(data);
    if (signed_count < 0) {
        return std::unexpected(ParseError::InvalidLength);
    }
    const size_t count = static_cast<size_t>(signed_count);
    if (count > (length - 2) / 4) {
        return std::unexpected(ParseError::Incomplete);
    }

    std::vector<std::optional<std::string_view>> values;
    values.reserve(count);
    size_t position = 2;
    for (size_t index = 0; index < count; ++index) {
        if (length - position < 4) {
            return std::unexpected(ParseError::Incomplete);
        }
        const int32_t value_length = read_signed_int32(data + position);
        position += 4;
        if (value_length == -1) {
            values.push_back(std::nullopt);
            continue;
        }
        if (value_length < -1) {
            return std::unexpected(ParseError::InvalidLength);
        }
        const size_t value_size = static_cast<size_t>(value_length);
        if (value_size > length - position) {
            return std::unexpected(ParseError::Incomplete);
        }
        values.emplace_back(std::string_view(data + position, value_size));
        position += value_size;
    }
    if (position != length) {
        return std::unexpected(ParseError::InvalidLength);
    }
    return values;
}

std::expected<PostgresRow, ParseError>
PostgresParser::parse_data_row(const char* data, size_t length) const
{
    auto views = parse_data_row_view(data, length);
    if (!views) {
        return std::unexpected(views.error());
    }
    std::vector<std::optional<std::string>> values;
    values.reserve(views->size());
    for (const auto& view : *views) {
        if (view.has_value()) {
            values.emplace_back(std::string(view->data(), view->size()));
        } else {
            values.push_back(std::nullopt);
        }
    }
    return PostgresRow(std::move(values));
}

std::expected<CommandCompleteInfo, ParseError>
PostgresParser::parse_command_complete(const char* data, size_t length) const
{
    size_t consumed = 0;
    auto tag = read_c_string(data, length, consumed);
    if (!tag) {
        return std::unexpected(tag.error());
    }
    if (consumed != length || tag->empty()) {
        return std::unexpected(ParseError::InvalidFormat);
    }

    CommandCompleteInfo info;
    info.tag = std::move(*tag);
    const size_t separator = info.tag.rfind(' ');
    const std::string_view numeric = separator == std::string::npos
        ? std::string_view(info.tag)
        : std::string_view(info.tag).substr(separator + 1);
    uint64_t affected_rows = 0;
    const auto parsed = std::from_chars(numeric.data(),
                                        numeric.data() + numeric.size(),
                                        affected_rows);
    if (parsed.ec == std::errc{} && parsed.ptr == numeric.data() + numeric.size()) {
        info.affected_rows = affected_rows;
    }
    return info;
}

std::expected<ReadyForQueryInfo, ParseError>
PostgresParser::parse_ready_for_query(const char* data, size_t length) const
{
    if (length < 1) {
        return std::unexpected(ParseError::Incomplete);
    }
    if (length != 1) {
        return std::unexpected(ParseError::InvalidLength);
    }
    if (data[0] != 'I' && data[0] != 'T' && data[0] != 'E') {
        return std::unexpected(ParseError::InvalidFormat);
    }
    return ReadyForQueryInfo{.transaction_status = data[0]};
}

std::expected<ParameterStatusInfo, ParseError>
PostgresParser::parse_parameter_status(const char* data, size_t length) const
{
    size_t first_consumed = 0;
    auto name = read_c_string(data, length, first_consumed);
    if (!name) {
        return std::unexpected(name.error());
    }
    size_t second_consumed = 0;
    auto value = read_c_string(data + first_consumed,
                             length - first_consumed,
                             second_consumed);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (first_consumed + second_consumed != length || name->empty()) {
        return std::unexpected(ParseError::InvalidFormat);
    }
    return ParameterStatusInfo{.name = std::move(*name), .value = std::move(*value)};
}

std::expected<BackendKeyDataInfo, ParseError>
PostgresParser::parse_backend_key_data(const char* data, size_t length) const
{
    if (length < 8) {
        return std::unexpected(ParseError::Incomplete);
    }
    if (length != 8) {
        return std::unexpected(ParseError::InvalidLength);
    }
    return BackendKeyDataInfo{
        .process_id = read_int32(data),
        .secret_key = read_int32(data + 4),
    };
}

std::expected<std::vector<uint32_t>, ParseError>
PostgresParser::parse_parameter_description(const char* data, size_t length) const
{
    if (length < 2) {
        return std::unexpected(ParseError::Incomplete);
    }
    const int16_t signed_count = read_signed_int16(data);
    if (signed_count < 0) {
        return std::unexpected(ParseError::InvalidLength);
    }
    const size_t count = static_cast<size_t>(signed_count);
    if (count > (length - 2) / 4) {
        return std::unexpected(ParseError::Incomplete);
    }
    if (length != 2 + count * 4) {
        return std::unexpected(ParseError::InvalidLength);
    }

    std::vector<uint32_t> oids;
    oids.reserve(count);
    size_t position = 2;
    for (size_t index = 0; index < count; ++index) {
        oids.push_back(read_int32(data + position));
        position += 4;
    }
    return oids;
}

std::expected<void, ParseError>
PostgresParser::parse_parse_complete(const char*, size_t length) const
{
    return parse_empty_payload(length);
}

std::expected<void, ParseError>
PostgresParser::parse_bind_complete(const char*, size_t length) const
{
    return parse_empty_payload(length);
}

std::expected<void, ParseError>
PostgresParser::parse_close_complete(const char*, size_t length) const
{
    return parse_empty_payload(length);
}

std::expected<void, ParseError>
PostgresParser::parse_no_data(const char*, size_t length) const
{
    return parse_empty_payload(length);
}

std::expected<void, ParseError>
PostgresParser::parse_portal_suspended(const char*, size_t length) const
{
    return parse_empty_payload(length);
}

std::string PostgresEncoder::wrap_message(char type, std::string_view payload) const
{
    if (!valid_message_payload_size(payload.size())) {
        return {};
    }
    std::string message;
    message.reserve(1 + kLengthFieldSize + payload.size());
    message.push_back(type);
    write_int32(message, static_cast<uint32_t>(kLengthFieldSize + payload.size()));
    message.append(payload.data(), payload.size());
    return message;
}

std::string PostgresEncoder::encode_startup_message(const PostgresConfig& config) const
{
    if (config.username.empty() || has_embedded_null(config.username) ||
        has_embedded_null(config.database) || has_embedded_null(config.application_name)) {
        return {};
    }

    std::string payload;
    write_int32(payload, kProtocolVersion3);
    write_c_string(payload, "user");
    write_c_string(payload, config.username);
    if (!config.database.empty()) {
        write_c_string(payload, "database");
        write_c_string(payload, config.database);
    }
    if (!config.application_name.empty()) {
        write_c_string(payload, "application_name");
        write_c_string(payload, config.application_name);
    }
    payload.push_back('\0');

    const size_t total_length = kLengthFieldSize + payload.size();
    if (total_length > kMaxStartupPacketLength ||
        total_length > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return {};
    }
    std::string message;
    message.reserve(total_length);
    write_int32(message, static_cast<uint32_t>(total_length));
    message.append(payload);
    return message;
}

std::string PostgresEncoder::encode_sasl_initial_response(std::string_view mechanism,
                                                       std::string_view client_first) const
{
    if (mechanism.empty() || has_embedded_null(mechanism) ||
        client_first.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return {};
    }
    std::string payload;
    write_c_string(payload, mechanism);
    write_int32(payload, static_cast<uint32_t>(client_first.size()));
    payload.append(client_first.data(), client_first.size());
    return wrap_message(kMsgPassword, payload);
}

std::string PostgresEncoder::encode_sasl_response(std::string_view client_final) const
{
    return wrap_message(kMsgPassword, client_final);
}

std::string PostgresEncoder::encode_password_message(std::string_view password) const
{
    if (has_embedded_null(password)) {
        return {};
    }
    std::string payload(password);
    payload.push_back('\0');
    return wrap_message(kMsgPassword, payload);
}

std::string PostgresEncoder::encode_query(std::string_view sql) const
{
    if (has_embedded_null(sql)) {
        return {};
    }
    std::string payload(sql);
    payload.push_back('\0');
    return wrap_message(kMsgQuery, payload);
}

std::string PostgresEncoder::encode_terminate() const
{
    return wrap_message(kMsgTerminate, {});
}

std::string PostgresEncoder::encode_parse(std::string_view statement_name,
                                         std::string_view sql,
                                         std::span<const uint32_t> parameter_type_oids) const
{
    if (has_embedded_null(statement_name) || has_embedded_null(sql) ||
        parameter_type_oids.size() >
            static_cast<size_t>(std::numeric_limits<int16_t>::max())) {
        return {};
    }
    std::string payload;
    write_c_string(payload, statement_name);
    write_c_string(payload, sql);
    write_int16(payload, static_cast<uint16_t>(parameter_type_oids.size()));
    for (uint32_t oid : parameter_type_oids) {
        write_int32(payload, oid);
    }
    return wrap_message(kMsgParse, payload);
}

std::string PostgresEncoder::encode_bind(
    std::string_view portal_name,
    std::string_view statement_name,
    std::span<const std::optional<std::string_view>> parameters) const
{
    auto payload = build_bind_payload(portal_name, statement_name, parameters);
    return payload ? wrap_message(kMsgBind, *payload) : std::string{};
}

std::string PostgresEncoder::encode_bind(
    std::string_view portal_name,
    std::string_view statement_name,
    std::span<const std::optional<std::string>> parameters) const
{
    auto payload = build_bind_payload(portal_name, statement_name, parameters);
    return payload ? wrap_message(kMsgBind, *payload) : std::string{};
}

std::string PostgresEncoder::encode_describe_statement(std::string_view statement_name) const
{
    if (has_embedded_null(statement_name)) {
        return {};
    }
    std::string payload(1, 'S');
    write_c_string(payload, statement_name);
    return wrap_message(kMsgDescribe, payload);
}

std::string PostgresEncoder::encode_describe_portal(std::string_view portal_name) const
{
    if (has_embedded_null(portal_name)) {
        return {};
    }
    std::string payload(1, 'P');
    write_c_string(payload, portal_name);
    return wrap_message(kMsgDescribe, payload);
}

std::string PostgresEncoder::encode_execute(std::string_view portal_name,
                                           uint32_t max_rows) const
{
    if (has_embedded_null(portal_name) ||
        max_rows > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return {};
    }
    std::string payload;
    write_c_string(payload, portal_name);
    write_int32(payload, max_rows);
    return wrap_message(kMsgExecute, payload);
}

std::string PostgresEncoder::encode_sync() const
{
    return wrap_message(kMsgSync, {});
}

std::string PostgresEncoder::encode_close_statement(std::string_view statement_name) const
{
    if (has_embedded_null(statement_name)) {
        return {};
    }
    std::string payload(1, 'S');
    write_c_string(payload, statement_name);
    return wrap_message(kMsgClose, payload);
}

std::string PostgresEncoder::encode_close_portal(std::string_view portal_name) const
{
    if (has_embedded_null(portal_name)) {
        return {};
    }
    std::string payload(1, 'P');
    write_c_string(payload, portal_name);
    return wrap_message(kMsgClose, payload);
}

} // namespace galay::postgres::protocol
