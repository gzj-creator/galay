/**
 * @file postgres_protocol.h
 * @brief PostgreSQL wire protocol v3 parser and encoder.
 */

#ifndef GALAY_POSTGRES_PROTOCOL_H
#define GALAY_POSTGRES_PROTOCOL_H

#include "postgres_packet.h"
#include "../base/postgres_config.h"
#include "../base/postgres_value.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galay::postgres::protocol
{

uint16_t read_int16(const char* data) noexcept;
uint32_t read_int32(const char* data) noexcept;
void write_int16(std::string& output, uint16_t value);
void write_int32(std::string& output, uint32_t value);
std::expected<std::string, ParseError> read_c_string(const char* data,
                                                   size_t length,
                                                   size_t& consumed);
void write_c_string(std::string& output, std::string_view value);

class PostgresParser
{
public:
    [[nodiscard]] std::expected<MessageHeader, ParseError>
    parse_header(const char* data, size_t length) const;

    /** The returned payload borrows data from the caller's buffer. */
    [[nodiscard]] std::expected<MessageView, ParseError>
    extract_message(const char* data, size_t length) const;

    [[nodiscard]] std::expected<AuthenticationRequest, ParseError>
    parse_authentication_request(const char* data, size_t length) const;
    [[nodiscard]] std::expected<ErrorFields, ParseError>
    parse_error_response(const char* data, size_t length) const;
    [[nodiscard]] std::expected<std::vector<RowDescriptionField>, ParseError>
    parse_row_description(const char* data, size_t length) const;
    [[nodiscard]] std::expected<PostgresRow, ParseError>
    parse_data_row(const char* data, size_t length) const;

    /** Returned values borrow the DataRow payload until that payload is consumed. */
    [[nodiscard]] std::expected<std::vector<std::optional<std::string_view>>, ParseError>
    parse_data_row_view(const char* data, size_t length) const;

    [[nodiscard]] std::expected<CommandCompleteInfo, ParseError>
    parse_command_complete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<ReadyForQueryInfo, ParseError>
    parse_ready_for_query(const char* data, size_t length) const;
    [[nodiscard]] std::expected<ParameterStatusInfo, ParseError>
    parse_parameter_status(const char* data, size_t length) const;
    [[nodiscard]] std::expected<BackendKeyDataInfo, ParseError>
    parse_backend_key_data(const char* data, size_t length) const;
    [[nodiscard]] std::expected<std::vector<uint32_t>, ParseError>
    parse_parameter_description(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parse_parse_complete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parse_bind_complete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parse_close_complete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parse_no_data(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parse_portal_suspended(const char* data, size_t length) const;
};

class PostgresEncoder
{
public:
    [[nodiscard]] std::string encode_startup_message(const PostgresConfig& config) const;
    [[nodiscard]] std::string encode_sasl_initial_response(std::string_view mechanism,
                                                        std::string_view client_first) const;
    [[nodiscard]] std::string encode_sasl_response(std::string_view client_final) const;
    [[nodiscard]] std::string encode_password_message(std::string_view password) const;
    [[nodiscard]] std::string encode_query(std::string_view sql) const;
    [[nodiscard]] std::string encode_terminate() const;

    [[nodiscard]] std::string encode_parse(std::string_view statement_name,
                                          std::string_view sql,
                                          std::span<const uint32_t> parameter_type_oids = {}) const;
    [[nodiscard]] std::string encode_bind(
        std::string_view portal_name,
        std::string_view statement_name,
        std::span<const std::optional<std::string_view>> parameters) const;
    [[nodiscard]] std::string encode_bind(
        std::string_view portal_name,
        std::string_view statement_name,
        std::span<const std::optional<std::string>> parameters) const;
    [[nodiscard]] std::string encode_describe_statement(std::string_view statement_name) const;
    [[nodiscard]] std::string encode_describe_portal(std::string_view portal_name) const;
    [[nodiscard]] std::string encode_execute(std::string_view portal_name,
                                            uint32_t max_rows = 0) const;
    [[nodiscard]] std::string encode_sync() const;
    [[nodiscard]] std::string encode_close_statement(std::string_view statement_name) const;
    [[nodiscard]] std::string encode_close_portal(std::string_view portal_name) const;

private:
    [[nodiscard]] std::string wrap_message(char type, std::string_view payload) const;
};

} // namespace galay::postgres::protocol

#endif // GALAY_POSTGRES_PROTOCOL_H
