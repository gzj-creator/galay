#include <galay/cpp/galay-postgres/protoc/builder.h>
#include <galay/cpp/galay-postgres/protoc/postgres_protocol.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace galay::postgres;
using namespace galay::postgres::protocol;

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void append_c_string(std::string& output, std::string_view value)
{
    output.append(value);
    output.push_back('\0');
}

void test_big_endian_helpers_and_c_string()
{
    std::string bytes;
    write_int16(bytes, 0x1234);
    write_int32(bytes, 0x56789abc);
    require(bytes == std::string("\x12\x34\x56\x78\x9a\xbc", 6),
            "integer writers must use network byte order");
    require(read_int16(bytes.data()) == 0x1234, "int16 big-endian read mismatch");
    require(read_int32(bytes.data() + 2) == 0x56789abc, "int32 big-endian read mismatch");

    const std::string text("alpha\0tail", 10);
    size_t consumed = 99;
    auto parsed = read_c_string(text.data(), text.size(), consumed);
    require(parsed && *parsed == "alpha" && consumed == 6, "CString parse mismatch");

    consumed = 99;
    auto incomplete = read_c_string("alpha", 5, consumed);
    require(!incomplete && incomplete.error() == ParseError::Incomplete,
            "unterminated CString must be incomplete");
    require(consumed == 99, "failed CString parse must not change consumed");
}

void test_message_extraction_boundaries()
{
    PostgresEncoder encoder;
    PostgresParser parser;
    const std::string query = encoder.encode_query("SELECT 1");
    require(query == std::string("Q\0\0\0\rSELECT 1\0", 14), "Query wire vector mismatch");

    auto header = parser.parse_header(query.data(), query.size());
    require(header && header->type == kMsgQuery && header->length == 13,
            "message header parse mismatch");

    for (size_t length = 0; length < query.size(); ++length) {
        auto partial = parser.extract_message(query.data(), length);
        require(!partial && partial.error() == ParseError::Incomplete,
                "every truncated message prefix must be incomplete");
    }

    auto complete = parser.extract_message(query.data(), query.size());
    require(complete && complete->type == kMsgQuery, "complete message extraction failed");
    require(complete->payload_len == 9, "message payload length must exclude the length word");
    require(complete->consumed == query.size(), "message consumed byte count mismatch");
    require(std::string_view(complete->payload, complete->payload_len) ==
                std::string_view("SELECT 1\0", 9),
            "message payload view mismatch");

    const std::string invalid_short("Q\0\0\0\3", 5);
    auto invalid = parser.extract_message(invalid_short.data(), invalid_short.size());
    require(!invalid && invalid.error() == ParseError::InvalidLength,
            "length words smaller than four must be rejected");

    const std::string invalid_signed("Q\x80\0\0\0", 5);
    invalid = parser.extract_message(invalid_signed.data(), invalid_signed.size());
    require(!invalid && invalid.error() == ParseError::InvalidLength,
            "protocol lengths above INT32_MAX must be rejected");
}

void test_startup_and_frontend_encoders()
{
    PostgresEncoder encoder;
    PostgresConfig config = PostgresConfig::create("127.0.0.1", 5432, "alice", "pw", "appdb");
    config.application_name = "galay-test";

    const std::string startup = encoder.encode_startup_message(config);
    require(!startup.empty(), "valid startup message failed to encode");
    require(read_int32(startup.data()) == startup.size(), "startup length must include itself");
    require(read_int32(startup.data() + 4) == kProtocolVersion3,
            "startup protocol version mismatch");
    std::string expected_parameters;
    append_c_string(expected_parameters, "user");
    append_c_string(expected_parameters, "alice");
    append_c_string(expected_parameters, "database");
    append_c_string(expected_parameters, "appdb");
    append_c_string(expected_parameters, "application_name");
    append_c_string(expected_parameters, "galay-test");
    expected_parameters.push_back('\0');
    require(std::string_view(startup).substr(8) == expected_parameters,
            "startup parameter vector mismatch");

    PostgresConfig invalid = config;
    invalid.username.clear();
    require(encoder.encode_startup_message(invalid).empty(), "startup requires a username");
    invalid = config;
    invalid.database = std::string("bad\0db", 6);
    require(encoder.encode_startup_message(invalid).empty(), "startup must reject embedded NULs");

    constexpr std::string_view kClientFirst = "n,,n=,r=clientnonce";
    const std::string initial = encoder.encode_sasl_initial_response(
        "SCRAM-SHA-256", kClientFirst);
    require(!initial.empty() && initial.front() == kMsgPassword,
            "SASL initial response type mismatch");
    require(read_int32(initial.data() + 1) == initial.size() - 1,
            "SASL initial response length mismatch");
    size_t mechanism_consumed = 0;
    auto mechanism = read_c_string(initial.data() + 5, initial.size() - 5, mechanism_consumed);
    require(mechanism && *mechanism == "SCRAM-SHA-256", "SASL mechanism mismatch");
    require(read_int32(initial.data() + 5 + mechanism_consumed) == kClientFirst.size(),
            "SASL initial payload length mismatch");

    const std::string response = encoder.encode_sasl_response("c=biws,r=nonce,p=proof");
    require(response.front() == kMsgPassword &&
                std::string_view(response).substr(5) == "c=biws,r=nonce,p=proof",
            "SASL response payload mismatch");

    const std::string password = encoder.encode_password_message("secret");
    require(std::string_view(password).substr(5) == std::string_view("secret\0", 7),
            "cleartext password encoding mismatch");

    require(encoder.encode_terminate() == std::string("X\0\0\0\4", 5),
            "Terminate wire vector mismatch");
}

void test_authentication_and_error_parsing()
{
    PostgresParser parser;

    std::string unknown_auth;
    write_int32(unknown_auth, 999);
    auto auth = parser.parse_authentication_request(unknown_auth.data(), unknown_auth.size());
    require(!auth && auth.error() == ParseError::InvalidType,
            "unknown authentication request kinds must fail");

    std::string sasl;
    write_int32(sasl, static_cast<uint32_t>(AuthRequestKind::Sasl));
    append_c_string(sasl, "SCRAM-SHA-256");
    append_c_string(sasl, "SCRAM-SHA-256-PLUS");
    sasl.push_back('\0');
    auth = parser.parse_authentication_request(sasl.data(), sasl.size());
    require(auth && auth->kind == AuthRequestKind::Sasl, "SASL auth kind mismatch");
    require(auth->mechanisms.size() == 2 && auth->mechanisms[0] == "SCRAM-SHA-256",
            "SASL mechanism list mismatch");

    std::string continuation;
    write_int32(continuation, static_cast<uint32_t>(AuthRequestKind::SaslContinue));
    continuation.append("r=nonce,s=c2FsdA==,i=4096");
    auth = parser.parse_authentication_request(continuation.data(), continuation.size());
    require(auth && auth->data == "r=nonce,s=c2FsdA==,i=4096",
            "SASL continuation bytes mismatch");

    std::string missing_list_terminator;
    write_int32(missing_list_terminator, static_cast<uint32_t>(AuthRequestKind::Sasl));
    missing_list_terminator.append("SCRAM-SHA-256");
    auth = parser.parse_authentication_request(missing_list_terminator.data(),
                                             missing_list_terminator.size());
    require(!auth && auth.error() == ParseError::Incomplete,
            "unterminated mechanism list must fail");

    std::string error;
    error.push_back('S'); append_c_string(error, "ERROR");
    error.push_back('V'); append_c_string(error, "ERROR-NONLOCALIZED");
    error.push_back('C'); append_c_string(error, "42601");
    error.push_back('M'); append_c_string(error, "syntax error");
    error.push_back('D'); append_c_string(error, "detail");
    error.push_back('H'); append_c_string(error, "hint");
    error.push_back('\0');
    auto fields = parser.parse_error_response(error.data(), error.size());
    require(fields && fields->severity == "ERROR-NONLOCALIZED", "V severity must win over S");
    require(fields->sql_state == "42601" && fields->message == "syntax error",
            "error response core fields mismatch");
    require(fields->detail == "detail" && fields->hint == "hint",
            "error response optional fields mismatch");

    error.pop_back();
    fields = parser.parse_error_response(error.data(), error.size());
    require(!fields && fields.error() == ParseError::Incomplete,
            "ErrorResponse requires its final zero byte");

    std::string duplicate_error;
    duplicate_error.push_back('M'); append_c_string(duplicate_error, "first");
    duplicate_error.push_back('M'); append_c_string(duplicate_error, "second");
    duplicate_error.push_back('\0');
    fields = parser.parse_error_response(duplicate_error.data(), duplicate_error.size());
    require(!fields && fields.error() == ParseError::InvalidFormat,
            "duplicate ErrorResponse fields must fail");

    const std::string trailing_error("Mmessage\0\0x", 11);
    fields = parser.parse_error_response(trailing_error.data(), trailing_error.size());
    require(!fields && fields.error() == ParseError::InvalidFormat,
            "ErrorResponse bytes after the final terminator must fail");
}

void test_rows_and_metadata_parsing()
{
    PostgresParser parser;

    std::string description;
    write_int16(description, 1);
    append_c_string(description, "answer");
    write_int32(description, 42);
    write_int16(description, 3);
    write_int32(description, static_cast<uint32_t>(PostgresOid::INT4));
    write_int16(description, 4);
    write_int32(description, std::bit_cast<uint32_t>(int32_t{-1}));
    write_int16(description, 0);
    auto columns = parser.parse_row_description(description.data(), description.size());
    require(columns && columns->size() == 1, "RowDescription parse failed");
    require((*columns)[0].name == "answer" && (*columns)[0].table_oid == 42,
            "RowDescription name/table metadata mismatch");
    require((*columns)[0].type_oid == static_cast<uint32_t>(PostgresOid::INT4) &&
                (*columns)[0].type_size == 4 && (*columns)[0].type_modifier == -1,
            "RowDescription type metadata mismatch");

    for (size_t length = 0; length < description.size(); ++length) {
        auto partial = parser.parse_row_description(description.data(), length);
        require(!partial, "truncated RowDescription unexpectedly parsed");
    }

    std::string invalid_description = description;
    invalid_description.back() = '\2';
    columns = parser.parse_row_description(invalid_description.data(), invalid_description.size());
    require(!columns && columns.error() == ParseError::InvalidFormat,
            "RowDescription must reject unknown format codes");

    invalid_description = description;
    invalid_description.push_back('\0');
    columns = parser.parse_row_description(invalid_description.data(), invalid_description.size());
    require(!columns && columns.error() == ParseError::InvalidLength,
            "RowDescription trailing bytes must fail");

    std::string data_row;
    write_int16(data_row, 3);
    write_int32(data_row, 5); data_row.append("alpha");
    write_int32(data_row, std::numeric_limits<uint32_t>::max());
    write_int32(data_row, 0);
    auto row_view = parser.parse_data_row_view(data_row.data(), data_row.size());
    require(row_view && row_view->size() == 3, "DataRow view parse failed");
    require((*row_view)[0] && *(*row_view)[0] == "alpha", "DataRow value mismatch");
    require((*row_view)[0]->data() == data_row.data() + 6,
            "DataRow view must borrow its packet buffer");
    require(!(*row_view)[1] && (*row_view)[2] && (*row_view)[2]->empty(),
            "DataRow NULL/empty distinction mismatch");

    auto row = parser.parse_data_row(data_row.data(), data_row.size());
    require(row && row->size() == 3 && row->get_string(0) == "alpha",
            "owned DataRow parse mismatch");
    data_row[6] = 'A';
    require((*row_view)[0]->front() == 'A', "DataRow view must remain borrowed");
    require(row->get_string(0) == "alpha", "owned DataRow must not borrow packet storage");

    for (size_t length = 0; length < data_row.size(); ++length) {
        auto partial = parser.parse_data_row_view(data_row.data(), length);
        require(!partial, "truncated DataRow unexpectedly parsed");
    }

    std::string invalid_length;
    write_int16(invalid_length, 1);
    write_int32(invalid_length, std::bit_cast<uint32_t>(int32_t{-2}));
    auto invalid = parser.parse_data_row_view(invalid_length.data(), invalid_length.size());
    require(!invalid && invalid.error() == ParseError::InvalidLength,
            "negative DataRow lengths other than -1 must fail");
}

void test_completion_and_session_metadata()
{
    PostgresParser parser;

    const std::string update("UPDATE 17\0", 10);
    auto command = parser.parse_command_complete(update.data(), update.size());
    require(command && command->tag == "UPDATE 17" && command->affected_rows == 17,
            "UPDATE command tag parse mismatch");

    const std::string insert("INSERT 0 9\0", 11);
    command = parser.parse_command_complete(insert.data(), insert.size());
    require(command && command->affected_rows == 9, "INSERT affected rows mismatch");

    const char ready = 'T';
    auto ready_info = parser.parse_ready_for_query(&ready, 1);
    require(ready_info && ready_info->transaction_status == 'T',
            "ReadyForQuery transaction status mismatch");
    const char invalid_ready = 'X';
    ready_info = parser.parse_ready_for_query(&invalid_ready, 1);
    require(!ready_info && ready_info.error() == ParseError::InvalidFormat,
            "invalid transaction status must fail");

    std::string status;
    append_c_string(status, "server_version");
    append_c_string(status, "16.4");
    auto parameter = parser.parse_parameter_status(status.data(), status.size());
    require(parameter && parameter->name == "server_version" && parameter->value == "16.4",
            "ParameterStatus parse mismatch");

    const std::string empty_status("\0value\0", 7);
    parameter = parser.parse_parameter_status(empty_status.data(), empty_status.size());
    require(!parameter && parameter.error() == ParseError::InvalidFormat,
            "ParameterStatus must reject an empty name");

    const std::string truncated_status("name\0value", 10);
    parameter = parser.parse_parameter_status(truncated_status.data(), truncated_status.size());
    require(!parameter && parameter.error() == ParseError::Incomplete,
            "ParameterStatus must reject an unterminated value");

    const std::string trailing_status("name\0value\0x", 12);
    parameter = parser.parse_parameter_status(trailing_status.data(), trailing_status.size());
    require(!parameter && parameter.error() == ParseError::InvalidFormat,
            "ParameterStatus trailing bytes must fail");

    std::string key_data;
    write_int32(key_data, 0x10203040);
    write_int32(key_data, 0xa0b0c0d0);
    auto backend = parser.parse_backend_key_data(key_data.data(), key_data.size());
    require(backend && backend->process_id == 0x10203040 &&
                backend->secret_key == 0xa0b0c0d0,
            "BackendKeyData parse mismatch");
    backend = parser.parse_backend_key_data(key_data.data(), key_data.size() - 1);
    require(!backend && backend.error() == ParseError::Incomplete,
            "truncated BackendKeyData must be incomplete");
    key_data.push_back('\0');
    backend = parser.parse_backend_key_data(key_data.data(), key_data.size());
    require(!backend && backend.error() == ParseError::InvalidLength,
            "BackendKeyData trailing bytes must fail");
}

void test_extended_query_codec()
{
    PostgresEncoder encoder;
    PostgresParser parser;

    const std::array<uint32_t, 1> parameter_oids{static_cast<uint32_t>(PostgresOid::INT4)};
    const std::string parse = encoder.encode_parse("find_one", "SELECT $1", parameter_oids);
    require(!parse.empty() && parse.front() == kMsgParse, "Parse message encoding failed");

    const std::array<std::optional<std::string_view>, 2> parameters{
        std::string_view("42"), std::nullopt};
    const std::string bind = encoder.encode_bind("", "find_one", parameters);
    require(!bind.empty() && bind.front() == kMsgBind, "Bind message encoding failed");
    require(encoder.encode_describe_statement("find_one").front() == kMsgDescribe,
            "Describe statement encoding failed");
    require(encoder.encode_execute("", 0).front() == kMsgExecute,
            "Execute message encoding failed");
    require(encoder.encode_sync() == std::string("S\0\0\0\4", 5),
            "Sync wire vector mismatch");
    require(encoder.encode_close_statement("find_one").front() == kMsgClose,
            "Close statement encoding failed");

    std::string parameter_description;
    write_int16(parameter_description, 2);
    write_int32(parameter_description, static_cast<uint32_t>(PostgresOid::INT4));
    write_int32(parameter_description, 91042);
    auto oids = parser.parse_parameter_description(parameter_description.data(),
                                                 parameter_description.size());
    require(oids && oids->size() == 2 && (*oids)[0] == 23 && (*oids)[1] == 91042,
            "ParameterDescription parse mismatch");

    require(parser.parse_parse_complete(nullptr, 0).has_value(),
            "empty ParseComplete must parse");
    require(parser.parse_bind_complete(nullptr, 0).has_value(),
            "empty BindComplete must parse");
    require(parser.parse_no_data(nullptr, 0).has_value(), "empty NoData must parse");
    require(parser.parse_portal_suspended(nullptr, 0).has_value(),
            "empty PortalSuspended must parse");
    const char unexpected = 0;
    require(!parser.parse_parse_complete(&unexpected, 1),
            "ParseComplete payload must be empty");
}

std::string_view payload_of(const PostgresParser& parser,
                           const std::string& encoded,
                           char expected_type)
{
    auto message = parser.extract_message(encoded.data(), encoded.size());
    require(message && message->type == expected_type && message->consumed == encoded.size(),
            "frontend message envelope mismatch");
    return std::string_view(message->payload, message->payload_len);
}

void test_extended_query_wire_vectors_and_boundaries()
{
    PostgresEncoder encoder;
    PostgresParser parser;

    const std::array<uint32_t, 2> parameter_oids{
        static_cast<uint32_t>(PostgresOid::INT4),
        static_cast<uint32_t>(PostgresOid::TEXT)};
    const std::string parse = encoder.encode_parse("stmt", "SELECT $1, $2", parameter_oids);
    std::string expected_parse_payload;
    append_c_string(expected_parse_payload, "stmt");
    append_c_string(expected_parse_payload, "SELECT $1, $2");
    write_int16(expected_parse_payload, 2);
    write_int32(expected_parse_payload, static_cast<uint32_t>(PostgresOid::INT4));
    write_int32(expected_parse_payload, static_cast<uint32_t>(PostgresOid::TEXT));
    require(payload_of(parser, parse, kMsgParse) == expected_parse_payload,
            "Parse payload vector mismatch");

    const std::array<std::optional<std::string_view>, 3> viewed_parameters{
        std::string_view("42"), std::nullopt, std::string_view{}};
    const std::string bind = encoder.encode_bind("portal", "stmt", viewed_parameters);
    std::string expected_bind_payload;
    append_c_string(expected_bind_payload, "portal");
    append_c_string(expected_bind_payload, "stmt");
    write_int16(expected_bind_payload, 0);
    write_int16(expected_bind_payload, 3);
    write_int32(expected_bind_payload, 2);
    expected_bind_payload.append("42");
    write_int32(expected_bind_payload, std::numeric_limits<uint32_t>::max());
    write_int32(expected_bind_payload, 0);
    write_int16(expected_bind_payload, 0);
    require(payload_of(parser, bind, kMsgBind) == expected_bind_payload,
            "Bind must distinguish text, NULL, and empty text using default text formats");

    const std::array<std::optional<std::string>, 3> owned_parameters{
        std::string("42"), std::nullopt, std::string{}};
    require(encoder.encode_bind("portal", "stmt", owned_parameters) == bind,
            "owned and borrowed Bind overloads must encode identically");

    const std::string describe_statement = encoder.encode_describe_statement("stmt");
    require(payload_of(parser, describe_statement, kMsgDescribe) ==
                std::string_view("Sstmt\0", 6),
            "Describe statement payload mismatch");
    const std::string describe_portal = encoder.encode_describe_portal("portal");
    require(payload_of(parser, describe_portal, kMsgDescribe) ==
                std::string_view("Pportal\0", 8),
            "Describe portal payload mismatch");

    const std::string execute = encoder.encode_execute("portal", 17);
    std::string expected_execute_payload;
    append_c_string(expected_execute_payload, "portal");
    write_int32(expected_execute_payload, 17);
    require(payload_of(parser, execute, kMsgExecute) == expected_execute_payload,
            "Execute payload mismatch");

    require(payload_of(parser, encoder.encode_close_statement("stmt"), kMsgClose) ==
                std::string_view("Sstmt\0", 6),
            "Close statement payload mismatch");
    require(payload_of(parser, encoder.encode_close_portal("portal"), kMsgClose) ==
                std::string_view("Pportal\0", 8),
            "Close portal payload mismatch");

    const std::string embedded_null("bad\0name", 8);
    require(encoder.encode_parse(embedded_null, "SELECT 1").empty(),
            "Parse must reject a NUL in the statement name");
    require(encoder.encode_parse("stmt", std::string_view("bad\0sql", 7)).empty(),
            "Parse must reject a NUL in SQL");
    require(encoder.encode_bind(embedded_null, "stmt", viewed_parameters).empty(),
            "Bind must reject a NUL in the portal name");
    require(encoder.encode_describe_statement(embedded_null).empty() &&
                encoder.encode_describe_portal(embedded_null).empty(),
            "Describe must reject embedded NUL names");
    require(encoder.encode_close_statement(embedded_null).empty() &&
                encoder.encode_close_portal(embedded_null).empty(),
            "Close must reject embedded NUL names");
    require(encoder.encode_execute(embedded_null, 0).empty(),
            "Execute must reject an embedded NUL portal name");
    require(encoder.encode_execute("", static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
                .size() == 10,
            "Execute must accept the largest signed protocol row limit");
    require(encoder.encode_execute("", static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1U)
                .empty(),
            "Execute must reject row limits above INT32_MAX");

    std::vector<uint32_t> maximum_parameter_oids(
        static_cast<size_t>(std::numeric_limits<int16_t>::max()),
        static_cast<uint32_t>(PostgresOid::TEXT));
    require(!encoder.encode_parse("max_params", "SELECT 1", maximum_parameter_oids).empty(),
            "Parse must accept INT16_MAX parameter OIDs");
    maximum_parameter_oids.push_back(static_cast<uint32_t>(PostgresOid::TEXT));
    require(encoder.encode_parse("too_many", "SELECT 1", maximum_parameter_oids).empty(),
            "Parse must reject more than INT16_MAX parameter OIDs");

    std::vector<std::optional<std::string_view>> maximum_parameters(
        static_cast<size_t>(std::numeric_limits<int16_t>::max()), std::nullopt);
    require(!encoder.encode_bind("", "stmt", maximum_parameters).empty(),
            "Bind must accept INT16_MAX parameters");
    maximum_parameters.push_back(std::nullopt);
    require(encoder.encode_bind("", "stmt", maximum_parameters).empty(),
            "Bind must reject more than INT16_MAX parameters");

    std::string malformed_parameter_description;
    write_int16(malformed_parameter_description, 2);
    write_int32(malformed_parameter_description, 23);
    auto truncated = parser.parse_parameter_description(malformed_parameter_description.data(),
                                                      malformed_parameter_description.size());
    require(!truncated && truncated.error() == ParseError::Incomplete,
            "truncated ParameterDescription must be incomplete");
    malformed_parameter_description.append(5, '\0');
    auto oversized = parser.parse_parameter_description(malformed_parameter_description.data(),
                                                      malformed_parameter_description.size());
    require(!oversized && oversized.error() == ParseError::InvalidLength,
            "ParameterDescription trailing bytes must fail");

    require(parser.parse_close_complete(nullptr, 0).has_value(),
            "empty CloseComplete must parse");
    require(!parser.parse_bind_complete("x", 1) &&
                !parser.parse_close_complete("x", 1) &&
                !parser.parse_no_data("x", 1) &&
                !parser.parse_portal_suspended("x", 1),
            "extended completion messages must reject payload bytes");
}

void test_command_builder_boundaries()
{
    PostgresCommandBuilder builder;
    require(builder.empty() && builder.size() == 0, "new command builder must be empty");

    const std::array<uint32_t, 1> parameter_oids{
        static_cast<uint32_t>(PostgresOid::INT4)};
    const std::array<std::optional<std::string_view>, 1> parameters{
        std::string_view("42")};
    builder.reserve(6, 128);
    builder.append_query("SELECT 1")
        .append_parse("find_one", "SELECT $1", parameter_oids)
        .append_bind("", "find_one", parameters)
        .append_describe_statement("find_one")
        .append_execute("", 0)
        .append_sync();

    const auto commands = builder.commands();
    require(commands.size() == 6 && builder.size() == 6,
            "builder must retain every frontend message");
    require(commands[0].kind == PostgresCommandKind::Query &&
                commands[1].kind == PostgresCommandKind::Parse &&
                commands[2].kind == PostgresCommandKind::Bind &&
                commands[3].kind == PostgresCommandKind::Describe &&
                commands[4].kind == PostgresCommandKind::Execute &&
                commands[5].kind == PostgresCommandKind::Sync,
            "builder command kinds mismatch");
    for (const auto& command : commands) {
        require(!command.encoded.empty(), "valid builder command must have encoded bytes");
    }

    auto batch = builder.build();
    require(batch.encoded == builder.encoded(), "build must copy the encoded batch");
    require(batch.expected_ready == 2,
            "only Query and Sync produce ReadyForQuery boundaries");
    auto batch_clone = batch.clone();
    require(batch_clone.encoded == batch.encoded &&
                batch_clone.encoded.data() != batch.encoded.data() &&
                batch_clone.expected_ready == batch.expected_ready,
            "batch clone must own independent encoded storage");

    auto cloned = builder.clone();
    const auto cloned_commands = cloned.commands();
    require(cloned.encoded() == builder.encoded() &&
                cloned.encoded().data() != builder.encoded().data(),
            "builder clone must own independent encoded storage");
    require(cloned_commands[0].encoded.data() == cloned.encoded().data(),
            "cloned command views must be rebound to cloned storage");

    PostgresCommandBuilder moved(std::move(cloned));
    require(cloned.empty(), "moved-from builder must be empty");
    require(moved.commands()[0].encoded.data() == moved.encoded().data(),
            "moved command views must be rebound to moved storage");
    auto released = moved.release();
    require(released.expected_ready == 2 && !released.encoded.empty(),
            "release must preserve encoded bytes and ReadyForQuery count");
    require(moved.empty() && moved.encoded().empty(),
            "release must reset the builder");

    PostgresCommandBuilder invalid;
    invalid.append_query(std::string_view("bad\0sql", 7));
    require(invalid.size() == 1 && invalid.commands()[0].encoded.empty(),
            "invalid commands must retain an empty command slot");
    require(invalid.build().encoded.empty(), "invalid builder must not produce a partial batch");
    auto invalid_release = invalid.release();
    require(invalid_release.encoded.empty() && invalid.empty(),
            "releasing an invalid builder must clear it without partial output");
}

} // namespace

int main()
{
    test_big_endian_helpers_and_c_string();
    test_message_extraction_boundaries();
    test_startup_and_frontend_encoders();
    test_authentication_and_error_parsing();
    test_rows_and_metadata_parsing();
    test_completion_and_session_metadata();
    test_extended_query_codec();
    test_extended_query_wire_vectors_and_boundaries();
    test_command_builder_boundaries();
    return EXIT_SUCCESS;
}
