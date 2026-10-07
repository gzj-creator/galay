#include "builder.h"

#include <utility>

namespace galay::postgres::protocol
{

namespace
{

bool produces_ready_for_query(PostgresCommandKind kind) noexcept
{
    return kind == PostgresCommandKind::Query || kind == PostgresCommandKind::Sync;
}

} // namespace

PostgresEncodedBatch::PostgresEncodedBatch(std::string encoded_data,
                                           size_t ready_count) noexcept
    : encoded(std::move(encoded_data))
    , expected_ready(ready_count)
{
}

PostgresEncodedBatch PostgresEncodedBatch::clone() const
{
    PostgresEncodedBatch copy;
    copy.encoded = encoded;
    copy.expected_ready = expected_ready;
    return copy;
}

PostgresCommandBuilder::PostgresCommandBuilder(PostgresCommandBuilder&& other) noexcept
    : m_encoded(std::move(other.m_encoded))
    , m_commands(std::move(other.m_commands))
    , m_command_views()
    , m_expected_ready(other.m_expected_ready)
    , m_views_dirty(true)
{
    other.clear();
}

PostgresCommandBuilder& PostgresCommandBuilder::operator=(PostgresCommandBuilder&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    m_encoded = std::move(other.m_encoded);
    m_commands = std::move(other.m_commands);
    m_command_views.clear();
    m_expected_ready = other.m_expected_ready;
    m_views_dirty = true;
    other.clear();
    return *this;
}

PostgresCommandBuilder PostgresCommandBuilder::clone() const
{
    PostgresCommandBuilder copy;
    copy.m_encoded = m_encoded;
    copy.m_commands = m_commands;
    copy.m_expected_ready = m_expected_ready;
    copy.m_views_dirty = true;
    return copy;
}

void PostgresCommandBuilder::clear() noexcept
{
    m_encoded.clear();
    m_commands.clear();
    m_command_views.clear();
    m_expected_ready = 0;
    m_views_dirty = true;
}

void PostgresCommandBuilder::reserve(size_t command_count, size_t encoded_bytes)
{
    m_encoded.reserve(encoded_bytes);
    m_commands.reserve(command_count);
    m_command_views.reserve(command_count);
    m_views_dirty = true;
}

PostgresCommandBuilder& PostgresCommandBuilder::append_query(std::string_view sql)
{
    return append_encoded(PostgresEncoder{}.encode_query(sql), PostgresCommandKind::Query);
}

PostgresCommandBuilder& PostgresCommandBuilder::append_parse(
    std::string_view statement_name,
    std::string_view sql,
    std::span<const uint32_t> parameter_type_oids)
{
    return append_encoded(PostgresEncoder{}.encode_parse(statement_name, sql, parameter_type_oids),
                         PostgresCommandKind::Parse);
}

PostgresCommandBuilder& PostgresCommandBuilder::append_bind(
    std::string_view portal_name,
    std::string_view statement_name,
    std::span<const std::optional<std::string_view>> parameters)
{
    return append_encoded(PostgresEncoder{}.encode_bind(portal_name, statement_name, parameters),
                         PostgresCommandKind::Bind);
}

PostgresCommandBuilder& PostgresCommandBuilder::append_bind(
    std::string_view portal_name,
    std::string_view statement_name,
    std::span<const std::optional<std::string>> parameters)
{
    return append_encoded(PostgresEncoder{}.encode_bind(portal_name, statement_name, parameters),
                         PostgresCommandKind::Bind);
}

PostgresCommandBuilder&
PostgresCommandBuilder::append_describe_statement(std::string_view statement_name)
{
    return append_encoded(PostgresEncoder{}.encode_describe_statement(statement_name),
                         PostgresCommandKind::Describe);
}

PostgresCommandBuilder&
PostgresCommandBuilder::append_describe_portal(std::string_view portal_name)
{
    return append_encoded(PostgresEncoder{}.encode_describe_portal(portal_name),
                         PostgresCommandKind::Describe);
}

PostgresCommandBuilder& PostgresCommandBuilder::append_execute(std::string_view portal_name,
                                                              uint32_t max_rows)
{
    return append_encoded(PostgresEncoder{}.encode_execute(portal_name, max_rows),
                         PostgresCommandKind::Execute);
}

PostgresCommandBuilder& PostgresCommandBuilder::append_sync()
{
    return append_encoded(PostgresEncoder{}.encode_sync(), PostgresCommandKind::Sync);
}

PostgresCommandBuilder&
PostgresCommandBuilder::append_close_statement(std::string_view statement_name)
{
    return append_encoded(PostgresEncoder{}.encode_close_statement(statement_name),
                         PostgresCommandKind::Close);
}

PostgresCommandBuilder& PostgresCommandBuilder::append_close_portal(std::string_view portal_name)
{
    return append_encoded(PostgresEncoder{}.encode_close_portal(portal_name),
                         PostgresCommandKind::Close);
}

std::span<const PostgresCommandView> PostgresCommandBuilder::commands() const
{
    rebuild_views_if_needed();
    return std::span<const PostgresCommandView>(m_command_views);
}

size_t PostgresCommandBuilder::size() const noexcept
{
    return m_commands.size();
}

bool PostgresCommandBuilder::empty() const noexcept
{
    return m_commands.empty();
}

const std::string& PostgresCommandBuilder::encoded() const noexcept
{
    return m_encoded;
}

PostgresEncodedBatch PostgresCommandBuilder::build() const
{
    if (has_invalid_command()) {
        return {};
    }
    return PostgresEncodedBatch(m_encoded, m_expected_ready);
}

PostgresEncodedBatch PostgresCommandBuilder::release()
{
    if (has_invalid_command()) {
        clear();
        return {};
    }

    PostgresEncodedBatch batch(std::move(m_encoded), m_expected_ready);
    clear();
    return batch;
}

PostgresCommandBuilder& PostgresCommandBuilder::append_encoded(std::string encoded,
                                                              PostgresCommandKind kind)
{
    if (encoded.empty()) {
        append_invalid(kind);
        return *this;
    }

    const size_t offset = m_encoded.size();
    m_encoded.append(encoded);
    m_commands.push_back(CommandMeta{
        .encoded = Slice{offset, encoded.size()},
        .kind = kind,
    });
    if (produces_ready_for_query(kind)) {
        ++m_expected_ready;
    }
    m_views_dirty = true;
    return *this;
}

void PostgresCommandBuilder::append_invalid(PostgresCommandKind kind)
{
    m_commands.push_back(CommandMeta{
        .encoded = Slice{m_encoded.size(), 0},
        .kind = kind,
    });
    m_views_dirty = true;
}

bool PostgresCommandBuilder::has_invalid_command() const noexcept
{
    for (const auto& command : m_commands) {
        if (command.encoded.length == 0) {
            return true;
        }
    }
    return false;
}

void PostgresCommandBuilder::rebuild_views_if_needed() const
{
    if (!m_views_dirty) {
        return;
    }

    m_command_views.resize(m_commands.size());
    for (size_t i = 0; i < m_commands.size(); ++i) {
        const auto& command = m_commands[i];
        std::string_view encoded;
        if (command.encoded.length != 0) {
            encoded = std::string_view(m_encoded.data() + command.encoded.offset,
                                       command.encoded.length);
        }
        m_command_views[i] = PostgresCommandView{
            .encoded = encoded,
            .kind = command.kind,
        };
    }
    m_views_dirty = false;
}

} // namespace galay::postgres::protocol
