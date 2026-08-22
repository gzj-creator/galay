#ifndef GALAY_POSTGRES_DETAILS_AWAITABLE_H
#define GALAY_POSTGRES_DETAILS_AWAITABLE_H

#include "../async/client.h"

#include <array>
#include <coroutine>
#include <memory>
#include <sys/uio.h>

namespace galay::postgres::details
{

template<RingBufferBackendStrategy Strategy>
class PostgresConnectAwaitable
    : public galay::kernel::ForwardingAwaitable<PostgresConnectAwaitable<Strategy>>
    , public galay::kernel::TimeoutSupport<PostgresConnectAwaitable<Strategy>>
{
public:
    friend class galay::kernel::ForwardingAwaitable<PostgresConnectAwaitable<Strategy>>;
    using Result = std::expected<std::optional<bool>, PostgresError>;

    PostgresConnectAwaitable(AsyncPostgresClient<Strategy>& client, PostgresConfig config);
    PostgresConnectAwaitable(PostgresConnectAwaitable&&) noexcept = default;
    PostgresConnectAwaitable& operator=(PostgresConnectAwaitable&&) noexcept = default;
    PostgresConnectAwaitable(const PostgresConnectAwaitable&) = delete;
    PostgresConnectAwaitable& operator=(const PostgresConnectAwaitable&) = delete;

    [[nodiscard]] bool isInvalid() const;

private:
    enum class AuthStage
    {
        SendStartup,
        AwaitAuthRequest,
        SendSASLInitial,
        AwaitSASLContinue,
        SendSASLFinal,
        AwaitSASLFinal,
        AwaitReadyForQuery,
    };

    enum class Phase
    {
        Invalid,
        Connect,
        StartupWrite,
        AuthRead,
        AuthWrite,
        StartupComplete,
        Done,
    };

    struct SharedState
    {
        SharedState(AsyncPostgresClient<Strategy>& client, PostgresConfig config);

        AsyncPostgresClient<Strategy>* client = nullptr;
        PostgresConfig config;
        galay::kernel::Host host;
        protocol::ScramSha256 scram;
        std::string outgoing;
        std::string parse_scratch;
        std::string client_nonce;
        std::array<struct iovec, 2> read_iovecs{};
        std::optional<Result> result;
        size_t sent = 0;
        size_t read_iov_count = 0;
        Phase phase = Phase::Connect;
        AuthStage auth_stage = AuthStage::SendStartup;
        bool authentication_ok = false;
        bool server_signature_verified = false;
    };

    struct Machine
    {
        using result_type = Result;
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);
        galay::kernel::MachineAction<result_type> advance();
        void onConnect(std::expected<void, galay::kernel::IOError> result);
        void onRead(std::expected<size_t, galay::kernel::IOError> result);
        void onWrite(std::expected<size_t, galay::kernel::IOError> result);

    private:
        bool prepareReadWindow();
        std::expected<bool, PostgresError> parseFromRingBuffer();
        void setError(PostgresError error) noexcept;
        void setIoError(const galay::kernel::IOError& error, PostgresErrorType fallback) noexcept;
        void completeSuccess() noexcept;

        std::shared_ptr<SharedState> m_state;
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;
    std::shared_ptr<SharedState> m_state;

private:
    InnerAwaitable m_inner;
};

template<RingBufferBackendStrategy Strategy>
class PostgresQueryAwaitable
    : public galay::kernel::ForwardingAwaitable<PostgresQueryAwaitable<Strategy>>
    , public galay::kernel::TimeoutSupport<PostgresQueryAwaitable<Strategy>>
{
public:
    friend class galay::kernel::ForwardingAwaitable<PostgresQueryAwaitable<Strategy>>;
    using Result = std::expected<std::optional<PostgresResultSet>, PostgresError>;

    PostgresQueryAwaitable(AsyncPostgresClient<Strategy>& client, std::string_view sql);
    PostgresQueryAwaitable(PostgresQueryAwaitable&&) noexcept = default;
    PostgresQueryAwaitable& operator=(PostgresQueryAwaitable&&) noexcept = default;
    PostgresQueryAwaitable(const PostgresQueryAwaitable&) = delete;
    PostgresQueryAwaitable& operator=(const PostgresQueryAwaitable&) = delete;

    [[nodiscard]] bool isInvalid() const;

private:
    enum class Phase { Invalid, SendCommand, Receiving, Done };

    struct SharedState
    {
        SharedState(AsyncPostgresClient<Strategy>& client, std::string_view sql);

        AsyncPostgresClient<Strategy>* client = nullptr;
        std::string encoded_cmd;
        PostgresResultSet result_set;
        std::optional<PostgresError> pending_error;
        std::string parse_scratch;
        std::array<struct iovec, 2> read_iovecs{};
        std::optional<Result> result;
        size_t sent = 0;
        size_t read_iov_count = 0;
        Phase phase = Phase::SendCommand;
    };

    struct Machine
    {
        using result_type = Result;
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);
        galay::kernel::MachineAction<result_type> advance();
        void onRead(std::expected<size_t, galay::kernel::IOError> result);
        void onWrite(std::expected<size_t, galay::kernel::IOError> result);

    private:
        bool prepareReadWindow();
        std::expected<bool, PostgresError> parseFromRingBuffer();
        void setError(PostgresError error) noexcept;
        void setIoError(const galay::kernel::IOError& error, PostgresErrorType fallback) noexcept;

        std::shared_ptr<SharedState> m_state;
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;
    std::shared_ptr<SharedState> m_state;

private:
    InnerAwaitable m_inner;
};

template<RingBufferBackendStrategy Strategy>
class PostgresPrepareAwaitable
    : public galay::kernel::ForwardingAwaitable<PostgresPrepareAwaitable<Strategy>>
    , public galay::kernel::TimeoutSupport<PostgresPrepareAwaitable<Strategy>>
{
public:
    friend class galay::kernel::ForwardingAwaitable<PostgresPrepareAwaitable<Strategy>>;
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

    using Result = std::expected<std::optional<PrepareResult>, PostgresError>;

    PostgresPrepareAwaitable(AsyncPostgresClient<Strategy>& client,
                             std::string_view name,
                             std::string_view sql,
                             std::span<const uint32_t> parameter_types);
    PostgresPrepareAwaitable(PostgresPrepareAwaitable&&) noexcept = default;
    PostgresPrepareAwaitable& operator=(PostgresPrepareAwaitable&&) noexcept = default;
    PostgresPrepareAwaitable(const PostgresPrepareAwaitable&) = delete;
    PostgresPrepareAwaitable& operator=(const PostgresPrepareAwaitable&) = delete;

    [[nodiscard]] bool isInvalid() const;

private:
    enum class Phase { Invalid, SendCommand, Receiving, Done };

    struct SharedState
    {
        SharedState(AsyncPostgresClient<Strategy>& client,
                    std::string_view name,
                    std::string_view sql,
                    std::span<const uint32_t> parameter_types);

        AsyncPostgresClient<Strategy>* client = nullptr;
        std::string encoded_cmd;
        PrepareResult prepare_result;
        std::optional<PostgresError> pending_error;
        std::string parse_scratch;
        std::array<struct iovec, 2> read_iovecs{};
        std::optional<Result> result;
        size_t sent = 0;
        size_t read_iov_count = 0;
        Phase phase = Phase::SendCommand;
    };

    struct Machine
    {
        using result_type = Result;
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);
        galay::kernel::MachineAction<result_type> advance();
        void onRead(std::expected<size_t, galay::kernel::IOError> result);
        void onWrite(std::expected<size_t, galay::kernel::IOError> result);

    private:
        bool prepareReadWindow();
        std::expected<bool, PostgresError> parseFromRingBuffer();
        void setError(PostgresError error) noexcept;
        void setIoError(const galay::kernel::IOError& error, PostgresErrorType fallback) noexcept;

        std::shared_ptr<SharedState> m_state;
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;
    std::shared_ptr<SharedState> m_state;

private:
    InnerAwaitable m_inner;
};

template<RingBufferBackendStrategy Strategy>
class PostgresExecuteAwaitable
    : public galay::kernel::ForwardingAwaitable<PostgresExecuteAwaitable<Strategy>>
    , public galay::kernel::TimeoutSupport<PostgresExecuteAwaitable<Strategy>>
{
public:
    friend class galay::kernel::ForwardingAwaitable<PostgresExecuteAwaitable<Strategy>>;
    using Result = std::expected<std::optional<PostgresResultSet>, PostgresError>;

    PostgresExecuteAwaitable(AsyncPostgresClient<Strategy>& client,
                             std::string_view name,
                             std::span<const std::optional<std::string_view>> params);
    PostgresExecuteAwaitable(PostgresExecuteAwaitable&&) noexcept = default;
    PostgresExecuteAwaitable& operator=(PostgresExecuteAwaitable&&) noexcept = default;
    PostgresExecuteAwaitable(const PostgresExecuteAwaitable&) = delete;
    PostgresExecuteAwaitable& operator=(const PostgresExecuteAwaitable&) = delete;

    [[nodiscard]] bool isInvalid() const;

private:
    enum class Phase { Invalid, SendCommand, Receiving, Done };

    struct SharedState
    {
        SharedState(AsyncPostgresClient<Strategy>& client,
                    std::string_view name,
                    std::span<const std::optional<std::string_view>> params);

        AsyncPostgresClient<Strategy>* client = nullptr;
        std::string encoded_cmd;
        PostgresResultSet result_set;
        std::optional<PostgresError> pending_error;
        std::string parse_scratch;
        std::array<struct iovec, 2> read_iovecs{};
        std::optional<Result> result;
        size_t sent = 0;
        size_t read_iov_count = 0;
        Phase phase = Phase::SendCommand;
    };

    struct Machine
    {
        using result_type = Result;
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);
        galay::kernel::MachineAction<result_type> advance();
        void onRead(std::expected<size_t, galay::kernel::IOError> result);
        void onWrite(std::expected<size_t, galay::kernel::IOError> result);

    private:
        bool prepareReadWindow();
        std::expected<bool, PostgresError> parseFromRingBuffer();
        void setError(PostgresError error) noexcept;
        void setIoError(const galay::kernel::IOError& error, PostgresErrorType fallback) noexcept;

        std::shared_ptr<SharedState> m_state;
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;
    std::shared_ptr<SharedState> m_state;

private:
    InnerAwaitable m_inner;
};

template<RingBufferBackendStrategy Strategy>
class PostgresPipelineAwaitable
    : public galay::kernel::ForwardingAwaitable<PostgresPipelineAwaitable<Strategy>>
    , public galay::kernel::TimeoutSupport<PostgresPipelineAwaitable<Strategy>>
{
public:
    friend class galay::kernel::ForwardingAwaitable<PostgresPipelineAwaitable<Strategy>>;
    using Result = std::expected<std::optional<std::vector<PostgresResultSet>>, PostgresError>;

    PostgresPipelineAwaitable(AsyncPostgresClient<Strategy>& client,
                              std::span<const protocol::PostgresCommandView> commands);
    PostgresPipelineAwaitable(PostgresPipelineAwaitable&&) noexcept = default;
    PostgresPipelineAwaitable& operator=(PostgresPipelineAwaitable&&) noexcept = default;
    PostgresPipelineAwaitable(const PostgresPipelineAwaitable&) = delete;
    PostgresPipelineAwaitable& operator=(const PostgresPipelineAwaitable&) = delete;

    [[nodiscard]] bool isInvalid() const;

private:
    enum class Phase { Invalid, SendCommands, Receiving, Done };

    struct SharedState
    {
        SharedState(AsyncPostgresClient<Strategy>& client,
                    std::span<const protocol::PostgresCommandView> commands);

        AsyncPostgresClient<Strategy>* client = nullptr;
        std::string encoded_commands;
        std::vector<PostgresResultSet> results;
        PostgresResultSet current_result;
        std::optional<PostgresError> first_error;
        std::string parse_scratch;
        std::array<struct iovec, 2> read_iovecs{};
        std::optional<Result> result;
        size_t expected_ready = 0;
        size_t completed_ready = 0;
        size_t sent = 0;
        size_t read_iov_count = 0;
        Phase phase = Phase::SendCommands;
    };

    struct Machine
    {
        using result_type = Result;
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);
        galay::kernel::MachineAction<result_type> advance();
        void onRead(std::expected<size_t, galay::kernel::IOError> result);
        void onWrite(std::expected<size_t, galay::kernel::IOError> result);

    private:
        bool prepareReadWindow();
        std::expected<bool, PostgresError> parseFromRingBuffer();
        void setError(PostgresError error) noexcept;
        void setIoError(const galay::kernel::IOError& error, PostgresErrorType fallback) noexcept;

        std::shared_ptr<SharedState> m_state;
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;
    std::shared_ptr<SharedState> m_state;

private:
    InnerAwaitable m_inner;
};

} // namespace galay::postgres::details

#endif // GALAY_POSTGRES_DETAILS_AWAITABLE_H
