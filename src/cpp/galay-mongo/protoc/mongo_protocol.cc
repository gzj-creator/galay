#include "mongo_protocol.h"
#include "crc32c.h"

namespace galay::mongo::protocol
{

namespace
{

constexpr int32_t kChecksumPresentFlag = 0x01;

int32_t read_int32_le(const char* p)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24));
}

uint32_t read_uint32_le(const char* p)
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

void write_int32_le_at(std::string& out, size_t pos, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out[pos + 0] = static_cast<char>(u & 0xFF);
    out[pos + 1] = static_cast<char>((u >> 8) & 0xFF);
    out[pos + 2] = static_cast<char>((u >> 16) & 0xFF);
    out[pos + 3] = static_cast<char>((u >> 24) & 0xFF);
}

void append_int32_le(std::string& out, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out.push_back(static_cast<char>(u & 0xFF));
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
    out.push_back(static_cast<char>((u >> 16) & 0xFF));
    out.push_back(static_cast<char>((u >> 24) & 0xFF));
}

} // namespace

MongoMessage MongoMessage::clone() const
{
    MongoMessage copy;
    copy.header = header;
    copy.body = body.clone();
    copy.flags = flags;
    return copy;
}

std::expected<std::string, std::string> MongoProtocol::encode_op_msg(int32_t request_id,
                                                                   const MongoDocument& body,
                                                                   int32_t flags)
{
    std::string out;
    auto appended = append_op_msg(out, request_id, body, flags);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    return out;
}

std::expected<void, std::string> MongoProtocol::append_op_msg(std::string& out,
                                                            int32_t request_id,
                                                            const MongoDocument& body,
                                                            int32_t flags)
{
    const size_t base = out.size();
    out.reserve(base + 16 + 4 + 1 + 64);
    out.resize(base + 16, '\0');

    append_int32_le(out, flags);
    out.push_back(static_cast<char>(0));
    auto body_appended = BsonCodec::append_document(out, body);
    if (!body_appended) {
        out.resize(base);
        return std::unexpected(body_appended.error());
    }

    write_int32_le_at(out, base + 0, static_cast<int32_t>(out.size() - base));
    write_int32_le_at(out, base + 4, request_id);
    write_int32_le_at(out, base + 8, 0); // responseTo for request
    write_int32_le_at(out, base + 12, kMongoOpMsg);
    return {};
}

std::expected<void, std::string> MongoProtocol::append_op_msg_with_database(std::string& out,
                                                                        int32_t request_id,
                                                                        const MongoDocument& body,
                                                                        std::string_view database,
                                                                        int32_t flags)
{
    const size_t base = out.size();
    out.reserve(base + 16 + 4 + 1 + 64 + database.size());
    out.resize(base + 16, '\0');

    append_int32_le(out, flags);
    out.push_back(static_cast<char>(0));
    auto body_appended = BsonCodec::append_document_with_database(out, body, database);
    if (!body_appended) {
        out.resize(base);
        return std::unexpected(body_appended.error());
    }

    write_int32_le_at(out, base + 0, static_cast<int32_t>(out.size() - base));
    write_int32_le_at(out, base + 4, request_id);
    write_int32_le_at(out, base + 8, 0); // responseTo for request
    write_int32_le_at(out, base + 12, kMongoOpMsg);
    return {};
}

std::expected<MongoMessage, MongoError> MongoProtocol::decode_message(const char* data, size_t len)
{
    if (data == nullptr || len < 16) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Mongo message too short"));
    }

    MongoMessage message;
    message.header.message_length = read_int32_le(data + 0);
    message.header.request_id = read_int32_le(data + 4);
    message.header.response_to = read_int32_le(data + 8);
    message.header.op_code = read_int32_le(data + 12);

    if (message.header.message_length < 21) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Invalid Mongo message length"));
    }

    if (static_cast<size_t>(message.header.message_length) > len) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Incomplete Mongo message"));
    }

    if (message.header.op_code == kMongoOpCompressed) {
        return std::unexpected(MongoError(MONGO_ERROR_UNSUPPORTED,
                                          "OP_COMPRESSED is not supported"));
    }

    if (message.header.op_code != kMongoOpMsg) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Unsupported Mongo opCode: " +
                                          std::to_string(message.header.op_code)));
    }

    size_t pos = 16;
    message.flags = read_int32_le(data + pos);
    pos += 4;

    // If checksumPresent bit is set, the last 4 bytes are a CRC32 checksum
    const size_t message_size = static_cast<size_t>(message.header.message_length);
    if (message.flags & kChecksumPresentFlag) {
        if (message_size < 25) {
            return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                              "OP_MSG checksum missing"));
        }

        const uint32_t expected_checksum = read_uint32_le(data + message_size - 4);
        const uint32_t actual_checksum = detail::crc32c(data, message_size - 4);
        if (actual_checksum != expected_checksum) {
            return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                              "OP_MSG checksum mismatch"));
        }
    }

    const size_t parseable_end = (message.flags & kChecksumPresentFlag)
        ? message_size - 4
        : message_size;

    bool body_found = false;

    while (pos < parseable_end) {
        const auto section_kind = static_cast<uint8_t>(data[pos++]);

        if (section_kind == 0) {
            size_t consumed = 0;
            auto document_or_err = BsonCodec::decode_document(
                data + pos,
                parseable_end - pos,
                consumed);
            if (!document_or_err) {
                return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                                  "Failed to decode OP_MSG body: " +
                                                  document_or_err.error()));
            }
            message.body = std::move(document_or_err.value());
            pos += consumed;
            body_found = true;
            continue;
        }

        if (section_kind == 1) {
            if (pos + 4 > parseable_end) {
                return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                                  "Invalid OP_MSG section(1) header"));
            }

            const int32_t section_size = read_int32_le(data + pos);
            if (section_size <= 4) {
                return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                                  "Invalid OP_MSG section(1) size"));
            }

            if (pos + static_cast<size_t>(section_size) > parseable_end) {
                return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                                  "Incomplete OP_MSG section(1)"));
            }

            pos += static_cast<size_t>(section_size);
            continue;
        }

        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Unknown OP_MSG section kind: " +
                                          std::to_string(section_kind)));
    }

    if (!body_found) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "OP_MSG body(section 0) missing"));
    }

    return std::move(message);
}

std::expected<MongoMessage, MongoError>
MongoProtocol::extract_message(const char* data, size_t len, size_t& consumed)
{
    if (data == nullptr || len < 4) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Mongo message header incomplete"));
    }

    const int32_t message_length = read_int32_le(data);
    if (message_length < 16) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Invalid Mongo message length"));
    }

    if (static_cast<size_t>(message_length) > len) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Incomplete Mongo message"));
    }

    consumed = static_cast<size_t>(message_length);
    return decode_message(data, consumed);
}

MongoDocument MongoProtocol::make_command(std::string db,
                                         std::string command_name,
                                         MongoValue command_value,
                                         MongoDocument arguments)
{
    MongoDocument command;
    command.append(std::move(command_name), std::move(command_value));

    for (auto& field : arguments.fields()) {
        command.append(std::move(field.first), std::move(field.second));
    }

    command.set("$db", std::move(db));
    return command;
}

} // namespace galay::mongo::protocol
