#include <iostream>
#include <cassert>
#include <cstring>
#include "galay-mysql/protoc/builder.h"
#include "galay-mysql/protoc/mysql_protocol.h"
#include "galay-mysql/protoc/mysql_packet.h"

using namespace galay::mysql::protocol;

void test_read_write_integers()
{
    std::cout << "Testing integer read/write..." << std::endl;

    // Test uint16
    {
        std::string buf;
        writeUint16(buf, 0x1234);
        assert(buf.size() == 2);
        assert(readUint16(buf.data()) == 0x1234);
    }

    // Test uint24
    {
        std::string buf;
        writeUint24(buf, 0x123456);
        assert(buf.size() == 3);
        assert(readUint24(buf.data()) == 0x123456);
    }

    // Test uint32
    {
        std::string buf;
        writeUint32(buf, 0x12345678);
        assert(buf.size() == 4);
        assert(readUint32(buf.data()) == 0x12345678);
    }

    // Test uint64
    {
        std::string buf;
        writeUint64(buf, 0x123456789ABCDEF0ULL);
        assert(buf.size() == 8);
        assert(readUint64(buf.data()) == 0x123456789ABCDEF0ULL);
    }

    std::cout << "  PASSED" << std::endl;
}

void test_len_enc_int()
{
    std::cout << "Testing length-encoded integer..." << std::endl;

    // 1-byte
    {
        std::string buf;
        writeLenEncInt(buf, 100);
        size_t consumed = 0;
        auto result = readLenEncInt(buf.data(), buf.size(), consumed);
        assert(result.has_value());
        assert(result.value() == 100);
        assert(consumed == 1);
    }

    // 2-byte (0xFC prefix)
    {
        std::string buf;
        writeLenEncInt(buf, 1000);
        size_t consumed = 0;
        auto result = readLenEncInt(buf.data(), buf.size(), consumed);
        assert(result.has_value());
        assert(result.value() == 1000);
        assert(consumed == 3);
    }

    // 3-byte (0xFD prefix)
    {
        std::string buf;
        writeLenEncInt(buf, 100000);
        size_t consumed = 0;
        auto result = readLenEncInt(buf.data(), buf.size(), consumed);
        assert(result.has_value());
        assert(result.value() == 100000);
        assert(consumed == 4);
    }

    // 8-byte (0xFE prefix)
    {
        std::string buf;
        writeLenEncInt(buf, 0x1000000ULL);
        size_t consumed = 0;
        auto result = readLenEncInt(buf.data(), buf.size(), consumed);
        assert(result.has_value());
        assert(result.value() == 0x1000000ULL);
        assert(consumed == 9);
    }

    std::cout << "  PASSED" << std::endl;
}

void test_len_enc_string()
{
    std::cout << "Testing length-encoded string..." << std::endl;

    std::string buf;
    writeLenEncString(buf, "hello");
    size_t consumed = 0;
    auto result = readLenEncString(buf.data(), buf.size(), consumed);
    assert(result.has_value());
    assert(result.value() == "hello");
    assert(consumed == 6); // 1 byte length + 5 bytes data

    std::cout << "  PASSED" << std::endl;
}

void test_packet_header()
{
    std::cout << "Testing packet header parse..." << std::endl;

    MysqlParser parser;

    // 构造一个包头: length=5, sequence_id=1
    std::string header;
    writeUint24(header, 5);
    header.push_back(0x01);

    auto result = parser.parseHeader(header.data(), header.size());
    assert(result.has_value());
    assert(result->length == 5);
    assert(result->sequence_id == 1);

    // 不完整的包头
    auto incomplete = parser.parseHeader(header.data(), 2);
    assert(!incomplete.has_value());
    assert(incomplete.error() == ParseError::Incomplete);

    std::cout << "  PASSED" << std::endl;
}

void test_encoder()
{
    std::cout << "Testing encoder..." << std::endl;

    MysqlEncoder encoder;

    // COM_QUERY
    auto query_pkt = encoder.encodeQuery("SELECT 1", 0);
    assert(query_pkt.size() > 4);
    // 包头: 3字节长度 + 1字节seq
    uint32_t payload_len = readUint24(query_pkt.data());
    assert(payload_len == 1 + 8); // 1 byte cmd + "SELECT 1"
    assert(static_cast<uint8_t>(query_pkt[4]) == static_cast<uint8_t>(CommandType::COM_QUERY));
    assert(query_pkt.substr(5) == "SELECT 1");

    // COM_QUIT
    auto quit_pkt = encoder.encodeQuit(0);
    assert(quit_pkt.size() == 5);
    assert(readUint24(quit_pkt.data()) == 1);
    assert(static_cast<uint8_t>(quit_pkt[4]) == static_cast<uint8_t>(CommandType::COM_QUIT));

    // COM_PING
    auto ping_pkt = encoder.encodePing(0);
    assert(ping_pkt.size() == 5);
    assert(static_cast<uint8_t>(ping_pkt[4]) == static_cast<uint8_t>(CommandType::COM_PING));

    std::cout << "  PASSED" << std::endl;
}

void test_command_builder()
{
    std::cout << "Testing command builder..." << std::endl;

    MysqlCommandBuilder builder;
    builder.reserve(3, 128);
    builder.appendQuery("SELECT 1");
    builder.appendPing();
    builder.appendInitDb("test_db");

    assert(builder.size() == 3);
    assert(!builder.empty());

    const auto views = builder.commands();
    assert(views.size() == 3);
    assert(views[0].kind == MysqlCommandKind::Query);
    assert(views[1].kind == MysqlCommandKind::Ping);
    assert(views[2].kind == MysqlCommandKind::InitDb);

    const std::string& encoded = builder.encoded();
    size_t offset = 0;

    auto assertCommand = [&](CommandType cmd, std::string_view payload) {
        assert(offset + MYSQL_PACKET_HEADER_SIZE <= encoded.size());
        const uint32_t packet_len = readUint24(encoded.data() + offset);
        assert(packet_len == payload.size() + 1);
        assert(static_cast<uint8_t>(encoded[offset + 4]) == static_cast<uint8_t>(cmd));
        if (!payload.empty()) {
            assert(encoded.substr(offset + 5, payload.size()) == payload);
        }
        offset += MYSQL_PACKET_HEADER_SIZE + packet_len;
    };

    assertCommand(CommandType::COM_QUERY, "SELECT 1");
    assertCommand(CommandType::COM_PING, "");
    assertCommand(CommandType::COM_INIT_DB, "test_db");
    assert(offset == encoded.size());

    auto released = builder.release();
    assert(builder.empty());
    assert(released.expected_responses == 3);
    assert(!released.encoded.empty());

    std::cout << "  PASSED" << std::endl;
}

void test_ok_packet_parse()
{
    std::cout << "Testing OK packet parse..." << std::endl;

    MysqlParser parser;

    // 构造一个简单的OK包: 0x00, affected_rows=1, last_insert_id=5, status=0x0002, warnings=0
    std::string payload;
    payload.push_back(0x00); // OK marker
    writeLenEncInt(payload, 1);  // affected_rows
    writeLenEncInt(payload, 5);  // last_insert_id
    writeUint16(payload, 0x0002); // status_flags (AUTOCOMMIT)
    writeUint16(payload, 0);      // warnings

    auto result = parser.parseOk(payload.data(), payload.size(), CLIENT_PROTOCOL_41);
    assert(result.has_value());
    assert(result->affected_rows == 1);
    assert(result->last_insert_id == 5);
    assert(result->status_flags == 0x0002);
    assert(result->warnings == 0);

    std::cout << "  PASSED" << std::endl;
}

void test_err_packet_parse()
{
    std::cout << "Testing ERR packet parse..." << std::endl;

    MysqlParser parser;

    // 构造ERR包: 0xFF, error_code=1045, '#', sql_state='28000', message
    std::string payload;
    payload.push_back(static_cast<char>(0xFF));
    writeUint16(payload, 1045);
    payload.push_back('#');
    payload.append("28000");
    payload.append("Access denied for user");

    auto result = parser.parseErr(payload.data(), payload.size(), CLIENT_PROTOCOL_41);
    assert(result.has_value());
    assert(result->error_code == 1045);
    assert(result->sql_state == "28000");
    assert(result->error_message == "Access denied for user");

    std::cout << "  PASSED" << std::endl;
}

void test_auth_switch_request_parse()
{
    std::cout << "Testing auth switch request parse..." << std::endl;

    MysqlParser parser;
    const std::string salt = "12345678901234567890";

    std::string payload;
    payload.push_back(static_cast<char>(0xFE));
    payload.append("mysql_native_password");
    payload.push_back('\0');
    payload.append(salt);
    payload.push_back('\0');

    auto result = parser.parseAuthSwitchRequest(payload.data(), payload.size());
    assert(result.has_value());
    assert(result->auth_plugin_name == "mysql_native_password");
    assert(result->auth_plugin_data == salt);

    std::string binary_salt_payload;
    binary_salt_payload.push_back(static_cast<char>(0xFE));
    binary_salt_payload.append("caching_sha2_password");
    binary_salt_payload.push_back('\0');
    binary_salt_payload.append("ab");
    binary_salt_payload.push_back('\0');
    binary_salt_payload.append("cd");
    binary_salt_payload.push_back('\0');
    auto binary_salt_result = parser.parseAuthSwitchRequest(binary_salt_payload.data(),
                                                            binary_salt_payload.size());
    assert(binary_salt_result.has_value());
    assert(binary_salt_result->auth_plugin_name == "caching_sha2_password");
    assert(binary_salt_result->auth_plugin_data.size() == 5);
    assert(binary_salt_result->auth_plugin_data == std::string("ab\0cd", 5));

    std::string no_salt;
    no_salt.push_back(static_cast<char>(0xFE));
    no_salt.append("mysql_native_password");
    no_salt.push_back('\0');
    auto no_salt_result = parser.parseAuthSwitchRequest(no_salt.data(), no_salt.size());
    assert(no_salt_result.has_value());
    assert(no_salt_result->auth_plugin_name == "mysql_native_password");
    assert(no_salt_result->auth_plugin_data.empty());

    const std::string empty_plugin = std::string(1, static_cast<char>(0xFE)) + '\0';
    auto empty_plugin_result = parser.parseAuthSwitchRequest(empty_plugin.data(), empty_plugin.size());
    assert(empty_plugin_result.has_value());
    assert(empty_plugin_result->auth_plugin_name.empty());
    assert(empty_plugin_result->auth_plugin_data.empty());

    std::string invalid = payload;
    invalid[0] = '\0';
    auto invalid_result = parser.parseAuthSwitchRequest(invalid.data(), invalid.size());
    assert(!invalid_result.has_value());
    assert(invalid_result.error() == ParseError::InvalidType);

    const std::string empty_payload;
    auto empty_result = parser.parseAuthSwitchRequest(empty_payload.data(), empty_payload.size());
    assert(!empty_result.has_value());
    assert(empty_result.error() == ParseError::Incomplete);

    const std::string marker_only(1, static_cast<char>(0xFE));
    auto marker_only_result = parser.parseAuthSwitchRequest(marker_only.data(), marker_only.size());
    assert(!marker_only_result.has_value());
    assert(marker_only_result.error() == ParseError::Incomplete);

    const std::string incomplete = std::string(1, static_cast<char>(0xFE)) + "mysql_native_password";
    auto incomplete_result = parser.parseAuthSwitchRequest(incomplete.data(), incomplete.size());
    assert(!incomplete_result.has_value());
    assert(incomplete_result.error() == ParseError::Incomplete);

    std::cout << "  PASSED" << std::endl;
}

int main()
{
    std::cout << "=== T1: MySQL Protocol Tests ===" << std::endl;

    test_read_write_integers();
    test_len_enc_int();
    test_len_enc_string();
    test_packet_header();
    test_encoder();
    test_command_builder();
    test_ok_packet_parse();
    test_err_packet_parse();
    test_auth_switch_request_parse();

    std::cout << "\nAll protocol tests PASSED!" << std::endl;
    return 0;
}
