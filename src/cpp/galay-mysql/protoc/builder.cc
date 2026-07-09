#include "builder.h"

namespace galay::mysql::protocol
{

MysqlEncodedBatch::MysqlEncodedBatch(std::string encoded_data, size_t response_count) noexcept
    : encoded(std::move(encoded_data))
    , expected_responses(response_count)
{
}

MysqlEncodedBatch MysqlEncodedBatch::clone() const
{
    MysqlEncodedBatch copy;
    copy.encoded = encoded;
    copy.expected_responses = expected_responses;
    return copy;
}

MysqlCommandBuilder::MysqlCommandBuilder(MysqlCommandBuilder&& other) noexcept
    : m_encoded(std::move(other.m_encoded))
    , m_commands(std::move(other.m_commands))
    , m_command_views()
    , m_views_dirty(true)
{
    other.clear();
}

MysqlCommandBuilder& MysqlCommandBuilder::operator=(MysqlCommandBuilder&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    m_encoded = std::move(other.m_encoded);
    m_commands = std::move(other.m_commands);
    m_command_views.clear();
    m_views_dirty = true;
    other.clear();
    return *this;
}

MysqlCommandBuilder MysqlCommandBuilder::clone() const
{
    MysqlCommandBuilder copy;
    copy.m_encoded = m_encoded;
    copy.m_commands = m_commands;
    copy.m_command_views.clear();
    copy.m_views_dirty = true;
    return copy;
}

void MysqlCommandBuilder::clear() noexcept
{
    m_encoded.clear();
    m_commands.clear();
    m_command_views.clear();
    m_views_dirty = true;
}

void MysqlCommandBuilder::reserve(size_t command_count, size_t encoded_bytes)
{
    m_encoded.reserve(encoded_bytes);
    m_commands.reserve(command_count);
    m_command_views.reserve(command_count);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendQuery(std::string_view sql, uint8_t sequence_id)
{
    return appendSimple(CommandType::COM_QUERY, sql, sequence_id, MysqlCommandKind::Query);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendStmtPrepare(std::string_view sql, uint8_t sequence_id)
{
    return appendSimple(CommandType::COM_STMT_PREPARE, sql, sequence_id, MysqlCommandKind::StmtPrepare);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendInitDb(std::string_view database, uint8_t sequence_id)
{
    return appendSimple(CommandType::COM_INIT_DB, database, sequence_id, MysqlCommandKind::InitDb);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendPing(uint8_t sequence_id)
{
    return appendSimple(CommandType::COM_PING, {}, sequence_id, MysqlCommandKind::Ping);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendQuit(uint8_t sequence_id)
{
    return appendSimple(CommandType::COM_QUIT, {}, sequence_id, MysqlCommandKind::Quit);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendResetConnection(uint8_t sequence_id)
{
    return appendSimple(CommandType::COM_RESET_CONNECTION,
                        {},
                        sequence_id,
                        MysqlCommandKind::ResetConnection);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendSimple(CommandType cmd,
                                                       std::string_view payload,
                                                       uint8_t sequence_id,
                                                       MysqlCommandKind kind)
{
    if (payload.size() > MYSQL_MAX_PACKET_SIZE - 1U) {
        appendInvalid(kind, sequence_id);
        return *this;
    }
    m_encoded.reserve(m_encoded.size() + estimateSimplePacketBytes(payload.size()));
    return appendFast(cmd, payload, sequence_id, kind);
}

MysqlCommandBuilder& MysqlCommandBuilder::appendFast(CommandType cmd,
                                                     std::string_view payload,
                                                     uint8_t sequence_id,
                                                     MysqlCommandKind kind)
{
    appendSimpleFast(cmd, payload, sequence_id, kind);
    return *this;
}

std::span<const MysqlCommandView> MysqlCommandBuilder::commands() const
{
    rebuildViewsIfNeeded();
    return std::span<const MysqlCommandView>(m_command_views);
}

size_t MysqlCommandBuilder::size() const noexcept
{
    return m_commands.size();
}

bool MysqlCommandBuilder::empty() const noexcept
{
    return m_commands.empty();
}

const std::string& MysqlCommandBuilder::encoded() const noexcept
{
    return m_encoded;
}

MysqlEncodedBatch MysqlCommandBuilder::build() const
{
    if (hasInvalidCommand()) {
        return MysqlEncodedBatch{};
    }
    return MysqlEncodedBatch(m_encoded, m_commands.size());
}

MysqlEncodedBatch MysqlCommandBuilder::release()
{
    if (hasInvalidCommand()) {
        m_encoded.clear();
        m_commands.clear();
        m_command_views.clear();
        m_views_dirty = true;
        return MysqlEncodedBatch{};
    }

    MysqlEncodedBatch out(std::move(m_encoded), m_commands.size());
    m_encoded.clear();
    m_commands.clear();
    m_command_views.clear();
    m_views_dirty = true;
    return out;
}

void MysqlCommandBuilder::appendPacketHeaderFast(std::string& out, uint32_t payload_len, uint8_t sequence_id)
{
    out.push_back(static_cast<char>(payload_len & 0xFF));
    out.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
    out.push_back(static_cast<char>((payload_len >> 16) & 0xFF));
    out.push_back(static_cast<char>(sequence_id));
}

size_t MysqlCommandBuilder::estimateSimplePacketBytes(size_t payload_size) noexcept
{
    return MYSQL_PACKET_HEADER_SIZE + 1 + payload_size;
}

void MysqlCommandBuilder::appendInvalid(MysqlCommandKind kind, uint8_t sequence_id)
{
    m_commands.push_back(CommandMeta{
        .encoded = Slice{m_encoded.size(), 0},
        .kind = kind,
        .sequence_id = sequence_id
    });
    m_views_dirty = true;
}

bool MysqlCommandBuilder::hasInvalidCommand() const noexcept
{
    for (const auto& command : m_commands) {
        if (command.encoded.length == 0) {
            return true;
        }
    }
    return false;
}

void MysqlCommandBuilder::appendSimpleFast(CommandType cmd,
                                           std::string_view payload,
                                           uint8_t sequence_id,
                                           MysqlCommandKind kind)
{
    if (payload.size() > MYSQL_MAX_PACKET_SIZE - 1U) {
        appendInvalid(kind, sequence_id);
        return;
    }
    const uint32_t payload_len = 1U + static_cast<uint32_t>(payload.size());
    const size_t begin = m_encoded.size();

    appendPacketHeaderFast(m_encoded, payload_len, sequence_id);
    m_encoded.push_back(static_cast<char>(cmd));
    if (!payload.empty()) {
        m_encoded.append(payload.data(), payload.size());
    }

    const size_t end = m_encoded.size();
    m_commands.push_back(CommandMeta{
        .encoded = Slice{begin, end - begin},
        .kind = kind,
        .sequence_id = sequence_id
    });
    m_views_dirty = true;
}

void MysqlCommandBuilder::rebuildViewsIfNeeded() const
{
    if (!m_views_dirty) {
        return;
    }

    m_command_views.resize(m_commands.size());
    for (size_t i = 0; i < m_commands.size(); ++i) {
        const auto& meta = m_commands[i];
        std::string_view encoded_view{};
        if (meta.encoded.length > 0) {
            encoded_view = std::string_view(
                m_encoded.data() + meta.encoded.offset,
                meta.encoded.length);
        }
        m_command_views[i] = MysqlCommandView{
            .encoded = encoded_view,
            .kind = meta.kind,
            .sequence_id = meta.sequence_id
        };
    }
    m_views_dirty = false;
}

} // namespace galay::mysql::protocol
