/**
 * @file postgres_packet.h
 * @brief PostgreSQL wire protocol v3 message constants and parsed values.
 */

#ifndef GALAY_POSTGRES_PACKET_H
#define GALAY_POSTGRES_PACKET_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace galay::postgres::protocol
{

inline constexpr uint32_t kProtocolVersion3 = 196608;
inline constexpr size_t kMessageHeaderSize = 5;
inline constexpr size_t kLengthFieldSize = 4;
inline constexpr size_t kMaxStartupPacketLength = 10000;

inline constexpr char kMsgPassword = 'p';
inline constexpr char kMsgQuery = 'Q';
inline constexpr char kMsgTerminate = 'X';
inline constexpr char kMsgParse = 'P';
inline constexpr char kMsgBind = 'B';
inline constexpr char kMsgDescribe = 'D';
inline constexpr char kMsgExecute = 'E';
inline constexpr char kMsgSync = 'S';
inline constexpr char kMsgClose = 'C';

inline constexpr char kMsgAuthentication = 'R';
inline constexpr char kMsgParameterStatus = 'S';
inline constexpr char kMsgBackendKeyData = 'K';
inline constexpr char kMsgReadyForQuery = 'Z';
inline constexpr char kMsgRowDescription = 'T';
inline constexpr char kMsgDataRow = 'D';
inline constexpr char kMsgCommandComplete = 'C';
inline constexpr char kMsgEmptyQueryResponse = 'I';
inline constexpr char kMsgErrorResponse = 'E';
inline constexpr char kMsgNoticeResponse = 'N';
inline constexpr char kMsgParseComplete = '1';
inline constexpr char kMsgBindComplete = '2';
inline constexpr char kMsgParameterDescription = 't';
inline constexpr char kMsgNoData = 'n';
inline constexpr char kMsgPortalSuspended = 's';
inline constexpr char kMsgCloseComplete = '3';

enum class AuthRequestKind : uint32_t
{
    Ok = 0,
    KerberosV5 = 2,
    CleartextPassword = 3,
    Md5Password = 5,
    Gss = 7,
    GssContinue = 8,
    Sspi = 9,
    Sasl = 10,
    SaslContinue = 11,
    SaslFinal = 12,
};

enum class ParseError
{
    Incomplete,
    InvalidFormat,
    InvalidType,
    InvalidLength,
    BufferOverflow,
};

struct MessageHeader
{
    char type = 0;
    uint32_t length = 0;
};

struct MessageView
{
    const char* payload = nullptr;
    size_t consumed = 0;
    uint32_t payload_len = 0;
    char type = 0;
};

struct AuthenticationRequest
{
    std::vector<std::string> mechanisms;
    std::string data;
    AuthRequestKind kind = AuthRequestKind::Ok;
};

struct RowDescriptionField
{
    std::string name;
    uint32_t table_oid = 0;
    uint32_t type_oid = 0;
    int32_t type_modifier = -1;
    int16_t column_index = 0;
    int16_t type_size = -1;
    int16_t format = 0;
};

struct ErrorFields
{
    std::string severity;
    std::string sql_state;
    std::string message;
    std::string detail;
    std::string hint;
    std::string position;
    std::string internal_position;
    std::string internal_query;
    std::string where;
    std::string schema_name;
    std::string table_name;
    std::string column_name;
    std::string data_type_name;
    std::string constraint_name;
    std::string file;
    std::string line;
    std::string routine;
};

struct CommandCompleteInfo
{
    std::string tag;
    uint64_t affected_rows = 0;
};

struct ReadyForQueryInfo
{
    char transaction_status = 'I';
};

struct ParameterStatusInfo
{
    std::string name;
    std::string value;
};

struct BackendKeyDataInfo
{
    uint32_t process_id = 0;
    uint32_t secret_key = 0;
};

} // namespace galay::postgres::protocol

#endif // GALAY_POSTGRES_PACKET_H
