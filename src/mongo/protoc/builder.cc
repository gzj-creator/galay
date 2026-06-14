#include "builder.h"

#include <algorithm>
#include <limits>

namespace galay::mongo::protocol
{

void MongoCommandBuilder::clear() noexcept
{
    m_commands.clear();
}

void MongoCommandBuilder::reserve(size_t command_count)
{
    m_commands.reserve(command_count);
}

MongoCommandBuilder& MongoCommandBuilder::append(MongoDocument command)
{
    m_commands.push_back(std::move(command));
    return *this;
}

MongoCommandBuilder& MongoCommandBuilder::append(std::string_view command_name,
                                                 MongoValue command_value,
                                                 MongoDocument arguments)
{
    MongoDocument command;
    command.append(std::string(command_name), std::move(command_value));
    auto& fields = arguments.fields();
    command.fields().reserve(1 + fields.size());
    for (auto& field : fields) {
        command.append(std::move(field.first), std::move(field.second));
    }
    return append(std::move(command));
}

MongoCommandBuilder& MongoCommandBuilder::appendPing()
{
    return append("ping", int32_t(1));
}

std::span<const MongoDocument> MongoCommandBuilder::commands() const noexcept
{
    return std::span<const MongoDocument>(m_commands.data(), m_commands.size());
}

size_t MongoCommandBuilder::size() const noexcept
{
    return m_commands.size();
}

bool MongoCommandBuilder::empty() const noexcept
{
    return m_commands.empty();
}

std::string MongoCommandBuilder::encodePipeline(std::string_view database,
                                                int32_t first_request_id,
                                                size_t reserve_per_command) const
{
    return encodePipeline(database, first_request_id, commands(), reserve_per_command);
}

std::string MongoCommandBuilder::encodePipeline(std::string_view database,
                                                int32_t first_request_id,
                                                std::span<const MongoDocument> commands,
                                                size_t reserve_per_command)
{
    if (commands.empty()) {
        return {};
    }

    const size_t reserve_hint = std::max<size_t>(32, reserve_per_command) * commands.size();
    std::string encoded;
    encoded.reserve(reserve_hint);

    if (first_request_id <= 0) {
        first_request_id = 1;
    }

    const int64_t max_request_id = std::numeric_limits<int32_t>::max();
    for (size_t i = 0; i < commands.size(); ++i) {
        int64_t request_id_i64 =
            static_cast<int64_t>(first_request_id) + static_cast<int64_t>(i);
        if (request_id_i64 > max_request_id) {
            request_id_i64 = 1 + ((request_id_i64 - 1) % max_request_id);
        }
        const int32_t request_id = static_cast<int32_t>(request_id_i64);

        const MongoDocument& command = commands[i];
        MongoProtocol::appendOpMsgWithDatabase(encoded, request_id, command, database);
    }

    return encoded;
}

} // namespace galay::mongo::protocol
