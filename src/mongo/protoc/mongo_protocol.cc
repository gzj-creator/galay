#include "mongo_protocol.h"

#include <cstring>

namespace galay::mongo::protocol
{

namespace
{

int32_t readInt32LE(const char* p)
{
    return static_cast<int32_t>(
        (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24));
}

void writeInt32LEAt(std::string& out, size_t pos, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out[pos + 0] = static_cast<char>(u & 0xFF);
    out[pos + 1] = static_cast<char>((u >> 8) & 0xFF);
    out[pos + 2] = static_cast<char>((u >> 16) & 0xFF);
    out[pos + 3] = static_cast<char>((u >> 24) & 0xFF);
}

void appendInt32LE(std::string& out, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    out.push_back(static_cast<char>(u & 0xFF));
    out.push_back(static_cast<char>((u >> 8) & 0xFF));
    out.push_back(static_cast<char>((u >> 16) & 0xFF));
    out.push_back(static_cast<char>((u >> 24) & 0xFF));
}

} // namespace

std::string MongoProtocol::encodeOpMsg(int32_t request_id,
                                       const MongoDocument& body,
                                       int32_t flags)
{
    std::string out;
    appendOpMsg(out, request_id, body, flags);
    return out;
}

void MongoProtocol::appendOpMsg(std::string& out,
                                int32_t request_id,
                                const MongoDocument& body,
                                int32_t flags)
{
    const size_t base = out.size();
    out.reserve(base + 16 + 4 + 1 + 64);
    out.resize(base + 16, '\0');

    appendInt32LE(out, flags);
    out.push_back(static_cast<char>(0));
    BsonCodec::appendDocument(out, body);

    writeInt32LEAt(out, base + 0, static_cast<int32_t>(out.size() - base));
    writeInt32LEAt(out, base + 4, request_id);
    writeInt32LEAt(out, base + 8, 0); // responseTo for request
    writeInt32LEAt(out, base + 12, kMongoOpMsg);
}

void MongoProtocol::appendOpMsgWithDatabase(std::string& out,
                                            int32_t request_id,
                                            const MongoDocument& body,
                                            std::string_view database,
                                            int32_t flags)
{
    const size_t base = out.size();
    out.reserve(base + 16 + 4 + 1 + 64 + database.size());
    out.resize(base + 16, '\0');

    appendInt32LE(out, flags);
    out.push_back(static_cast<char>(0));
    BsonCodec::appendDocumentWithDatabase(out, body, database);

    writeInt32LEAt(out, base + 0, static_cast<int32_t>(out.size() - base));
    writeInt32LEAt(out, base + 4, request_id);
    writeInt32LEAt(out, base + 8, 0); // responseTo for request
    writeInt32LEAt(out, base + 12, kMongoOpMsg);
}

std::expected<MongoMessage, MongoError> MongoProtocol::decodeMessage(const char* data, size_t len)
{
    if (data == nullptr || len < 16) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Mongo message too short"));
    }

    MongoMessage message;
    message.header.message_length = readInt32LE(data + 0);
    message.header.request_id = readInt32LE(data + 4);
    message.header.response_to = readInt32LE(data + 8);
    message.header.op_code = readInt32LE(data + 12);

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
    message.flags = readInt32LE(data + pos);
    pos += 4;

    // If checksumPresent bit is set, the last 4 bytes are a CRC32 checksum
    const size_t parseable_end = (message.flags & 0x01)
        ? static_cast<size_t>(message.header.message_length) - 4
        : static_cast<size_t>(message.header.message_length);

    bool body_found = false;

    while (pos < parseable_end) {
        const auto section_kind = static_cast<uint8_t>(data[pos++]);

        if (section_kind == 0) {
            size_t consumed = 0;
            auto document_or_err = BsonCodec::decodeDocument(
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

            const int32_t section_size = readInt32LE(data + pos);
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

    return message;
}

std::expected<MongoMessage, MongoError>
MongoProtocol::extractMessage(const char* data, size_t len, size_t& consumed)
{
    if (data == nullptr || len < 4) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Mongo message header incomplete"));
    }

    const int32_t message_length = readInt32LE(data);
    if (message_length < 16) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Invalid Mongo message length"));
    }

    if (static_cast<size_t>(message_length) > len) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL, "Incomplete Mongo message"));
    }

    consumed = static_cast<size_t>(message_length);
    return decodeMessage(data, consumed);
}

MongoDocument MongoProtocol::makeCommand(std::string db,
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
