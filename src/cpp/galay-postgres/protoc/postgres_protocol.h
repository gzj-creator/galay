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

uint16_t readInt16(const char* data) noexcept;
uint32_t readInt32(const char* data) noexcept;
void writeInt16(std::string& output, uint16_t value);
void writeInt32(std::string& output, uint32_t value);
std::expected<std::string, ParseError> readCString(const char* data,
                                                   size_t length,
                                                   size_t& consumed);
void writeCString(std::string& output, std::string_view value);

class PostgresParser
{
public:
    [[nodiscard]] std::expected<MessageHeader, ParseError>
    parseHeader(const char* data, size_t length) const;

    /** The returned payload borrows data from the caller's buffer. */
    [[nodiscard]] std::expected<MessageView, ParseError>
    extractMessage(const char* data, size_t length) const;

    [[nodiscard]] std::expected<AuthenticationRequest, ParseError>
    parseAuthenticationRequest(const char* data, size_t length) const;
    [[nodiscard]] std::expected<ErrorFields, ParseError>
    parseErrorResponse(const char* data, size_t length) const;
    [[nodiscard]] std::expected<std::vector<RowDescriptionField>, ParseError>
    parseRowDescription(const char* data, size_t length) const;
    [[nodiscard]] std::expected<PostgresRow, ParseError>
    parseDataRow(const char* data, size_t length) const;

    /** Returned values borrow the DataRow payload until that payload is consumed. */
    [[nodiscard]] std::expected<std::vector<std::optional<std::string_view>>, ParseError>
    parseDataRowView(const char* data, size_t length) const;

    [[nodiscard]] std::expected<CommandCompleteInfo, ParseError>
    parseCommandComplete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<ReadyForQueryInfo, ParseError>
    parseReadyForQuery(const char* data, size_t length) const;
    [[nodiscard]] std::expected<ParameterStatusInfo, ParseError>
    parseParameterStatus(const char* data, size_t length) const;
    [[nodiscard]] std::expected<BackendKeyDataInfo, ParseError>
    parseBackendKeyData(const char* data, size_t length) const;
    [[nodiscard]] std::expected<std::vector<uint32_t>, ParseError>
    parseParameterDescription(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parseParseComplete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parseBindComplete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parseCloseComplete(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parseNoData(const char* data, size_t length) const;
    [[nodiscard]] std::expected<void, ParseError>
    parsePortalSuspended(const char* data, size_t length) const;
};

class PostgresEncoder
{
public:
    [[nodiscard]] std::string encodeStartupMessage(const PostgresConfig& config) const;
    [[nodiscard]] std::string encodeSASLInitialResponse(std::string_view mechanism,
                                                        std::string_view client_first) const;
    [[nodiscard]] std::string encodeSASLResponse(std::string_view client_final) const;
    [[nodiscard]] std::string encodePasswordMessage(std::string_view password) const;
    [[nodiscard]] std::string encodeQuery(std::string_view sql) const;
    [[nodiscard]] std::string encodeTerminate() const;

    [[nodiscard]] std::string encodeParse(std::string_view statement_name,
                                          std::string_view sql,
                                          std::span<const uint32_t> parameter_type_oids = {}) const;
    [[nodiscard]] std::string encodeBind(
        std::string_view portal_name,
        std::string_view statement_name,
        std::span<const std::optional<std::string_view>> parameters) const;
    [[nodiscard]] std::string encodeBind(
        std::string_view portal_name,
        std::string_view statement_name,
        std::span<const std::optional<std::string>> parameters) const;
    [[nodiscard]] std::string encodeDescribeStatement(std::string_view statement_name) const;
    [[nodiscard]] std::string encodeDescribePortal(std::string_view portal_name) const;
    [[nodiscard]] std::string encodeExecute(std::string_view portal_name,
                                            uint32_t max_rows = 0) const;
    [[nodiscard]] std::string encodeSync() const;
    [[nodiscard]] std::string encodeCloseStatement(std::string_view statement_name) const;
    [[nodiscard]] std::string encodeClosePortal(std::string_view portal_name) const;

private:
    [[nodiscard]] std::string wrapMessage(char type, std::string_view payload) const;
};

} // namespace galay::postgres::protocol

#endif // GALAY_POSTGRES_PROTOCOL_H
