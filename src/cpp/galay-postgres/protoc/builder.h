/**
 * @file builder.h
 * @brief PostgreSQL frontend-message batch builder.
 */

#ifndef GALAY_POSTGRES_PROTOCOL_BUILDER_H
#define GALAY_POSTGRES_PROTOCOL_BUILDER_H

#include "postgres_protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galay::postgres::protocol
{

enum class PostgresCommandKind : uint8_t
{
    Raw = 0,
    Query,
    Parse,
    Bind,
    Describe,
    Execute,
    Sync,
    Close,
};

/**
 * A non-owning view into an encoded frontend message. The view remains valid
 * only until its originating builder is mutated, moved, released, or destroyed.
 */
struct PostgresCommandView
{
    std::string_view encoded;
    PostgresCommandKind kind = PostgresCommandKind::Raw;
};

class PostgresEncodedBatch
{
public:
    PostgresEncodedBatch() = default;
    PostgresEncodedBatch(std::string encoded_data, size_t ready_count) noexcept;
    PostgresEncodedBatch(PostgresEncodedBatch&&) noexcept = default;
    PostgresEncodedBatch& operator=(PostgresEncodedBatch&&) noexcept = default;

    [[nodiscard]] PostgresEncodedBatch clone() const;

    std::string encoded;
    size_t expected_ready = 0;

private:
    PostgresEncodedBatch(const PostgresEncodedBatch&) = delete;
    PostgresEncodedBatch& operator=(const PostgresEncodedBatch&) = delete;
};

/**
 * Builds a contiguous sequence of PostgreSQL frontend messages. Encoding
 * failures are retained as empty command slots, causing build() and release()
 * to return an empty batch instead of a partial wire sequence.
 */
class PostgresCommandBuilder
{
public:
    PostgresCommandBuilder() = default;
    PostgresCommandBuilder(PostgresCommandBuilder&& other) noexcept;
    PostgresCommandBuilder& operator=(PostgresCommandBuilder&& other) noexcept;

    [[nodiscard]] PostgresCommandBuilder clone() const;

    void clear() noexcept;
    void reserve(size_t command_count, size_t encoded_bytes);

    PostgresCommandBuilder& appendQuery(std::string_view sql);
    PostgresCommandBuilder& appendParse(
        std::string_view statement_name,
        std::string_view sql,
        std::span<const uint32_t> parameter_type_oids = {});
    PostgresCommandBuilder& appendBind(
        std::string_view portal_name,
        std::string_view statement_name,
        std::span<const std::optional<std::string_view>> parameters);
    PostgresCommandBuilder& appendBind(
        std::string_view portal_name,
        std::string_view statement_name,
        std::span<const std::optional<std::string>> parameters);
    PostgresCommandBuilder& appendDescribeStatement(std::string_view statement_name);
    PostgresCommandBuilder& appendDescribePortal(std::string_view portal_name);
    PostgresCommandBuilder& appendExecute(std::string_view portal_name,
                                          uint32_t max_rows = 0);
    PostgresCommandBuilder& appendSync();
    PostgresCommandBuilder& appendCloseStatement(std::string_view statement_name);
    PostgresCommandBuilder& appendClosePortal(std::string_view portal_name);

    [[nodiscard]] std::span<const PostgresCommandView> commands() const;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] const std::string& encoded() const noexcept;
    [[nodiscard]] PostgresEncodedBatch build() const;
    [[nodiscard]] PostgresEncodedBatch release();

private:
    struct Slice
    {
        size_t offset = 0;
        size_t length = 0;
    };

    struct CommandMeta
    {
        Slice encoded;
        PostgresCommandKind kind = PostgresCommandKind::Raw;
    };

    PostgresCommandBuilder& appendEncoded(std::string encoded, PostgresCommandKind kind);
    void appendInvalid(PostgresCommandKind kind);
    [[nodiscard]] bool hasInvalidCommand() const noexcept;
    void rebuildViewsIfNeeded() const;

    std::string m_encoded;
    std::vector<CommandMeta> m_commands;
    mutable std::vector<PostgresCommandView> m_command_views;
    size_t m_expected_ready = 0;
    mutable bool m_views_dirty = true;

    PostgresCommandBuilder(const PostgresCommandBuilder&) = delete;
    PostgresCommandBuilder& operator=(const PostgresCommandBuilder&) = delete;
};

} // namespace galay::postgres::protocol

#endif // GALAY_POSTGRES_PROTOCOL_BUILDER_H
