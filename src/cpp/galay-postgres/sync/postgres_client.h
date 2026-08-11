#ifndef GALAY_POSTGRES_SYNC_CLIENT_H
#define GALAY_POSTGRES_SYNC_CLIENT_H

#include "../base/postgres_config.h"
#include "../base/postgres_error.h"
#include "../base/postgres_value.h"
#include "../protoc/builder.h"
#include "../protoc/postgres_auth.h"
#include "../protoc/postgres_protocol.h"
#include "../../galay-utils/cache/ring_buffer.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::postgres
{

using PostgresResult = std::expected<PostgresResultSet, PostgresError>;
using PostgresVoidResult = std::expected<void, PostgresError>;
using PostgresBatchResult = std::expected<std::vector<PostgresResultSet>, PostgresError>;

class PostgresClient
{
public:
    struct PrepareResult
    {
        PrepareResult() = default;
        PrepareResult(PrepareResult&&) noexcept = default;
        PrepareResult& operator=(PrepareResult&&) noexcept = default;
        [[nodiscard]] PrepareResult clone() const;

        std::string statement_name;
        std::vector<uint32_t> parameter_types;
        std::vector<PostgresField> fields;

    private:
        PrepareResult(const PrepareResult&) = delete;
        PrepareResult& operator=(const PrepareResult&) = delete;
    };

    PostgresClient();
    ~PostgresClient();
    PostgresClient(PostgresClient&& other) noexcept;
    PostgresClient& operator=(PostgresClient&& other) noexcept;
    PostgresClient(const PostgresClient&) = delete;
    PostgresClient& operator=(const PostgresClient&) = delete;

    PostgresVoidResult connect(const PostgresConfig& config);
    PostgresVoidResult connect(const std::string& host,
                               uint16_t port,
                               const std::string& user,
                               const std::string& password,
                               const std::string& database = {});

    PostgresResult query(std::string_view sql);
    PostgresBatchResult batch(std::span<const protocol::PostgresCommandView> commands);
    PostgresBatchResult pipeline(std::span<const std::string_view> sqls);

    std::expected<PrepareResult, PostgresError> prepare(
        std::string_view name,
        std::string_view sql,
        std::span<const uint32_t> parameter_types = {});
    PostgresResult execute(std::string_view name,
                           const std::vector<std::optional<std::string>>& params);
    PostgresVoidResult closePrepared(std::string_view name);

    PostgresVoidResult beginTransaction();
    PostgresVoidResult commit();
    PostgresVoidResult rollback();
    PostgresVoidResult ping();

    void close() noexcept;
    [[nodiscard]] bool isConnected() const noexcept { return m_connected; }
    [[nodiscard]] char transactionStatus() const noexcept { return m_transaction_status; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& serverParameters() const noexcept
    {
        return m_server_parameters;
    }
    [[nodiscard]] const std::optional<protocol::BackendKeyDataInfo>& backendKeyData() const noexcept
    {
        return m_backend_key_data;
    }

private:
    struct Message
    {
        char type = 0;
        std::string payload;
    };

    static constexpr size_t kRecvBufferCapacity = 256 * 1024;

    PostgresVoidResult connectSocket(const std::string& host,
                                     uint16_t port,
                                     uint32_t timeout_ms,
                                     bool tcp_no_delay);
    void closeSocket() noexcept;
    PostgresVoidResult sendAll(std::string_view data);
    PostgresVoidResult sendAllv(std::span<const struct iovec> iovecs);
    PostgresVoidResult recvIntoRingBuffer();
    std::expected<std::optional<Message>, PostgresError> tryExtractMessage();
    std::expected<Message, PostgresError> recvMessage();
    PostgresResult receiveResultUntilReady();
    PostgresVoidResult runSimpleStatement(std::string_view sql);

    galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap,
                             std::dynamic_extent> m_recv_ring_buffer;
    std::unordered_map<std::string, std::string> m_server_parameters;
    std::string m_parse_scratch;
    std::optional<protocol::BackendKeyDataInfo> m_backend_key_data;
    protocol::PostgresParser m_parser;
    protocol::PostgresEncoder m_encoder;
    int m_socket_fd = -1;
    char m_transaction_status = 'I';
    bool m_connected = false;
};

} // namespace galay::postgres

#endif // GALAY_POSTGRES_SYNC_CLIENT_H
