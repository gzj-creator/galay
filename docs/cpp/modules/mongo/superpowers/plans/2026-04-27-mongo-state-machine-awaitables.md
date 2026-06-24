# Mongo State Machine Awaitables Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert `galay-mongo` async connect, command, and pipeline awaitables from coroutine `Task` implementations to explicit kernel state-machine awaitables while keeping public result semantics unchanged.

**Architecture:** `async_mongo_client.h` will define three value awaitables: `MongoConnectAwaitable`, `MongoCommandAwaitable`, and `MongoPipelineAwaitable`. Each awaitable owns a `SharedState` plus `galay::kernel::StateMachineAwaitable<Machine>`, and each machine drives connect/read/write via `MachineAction` just like `galay-http`, `galay-redis`, and `galay-mysql`.

**Tech Stack:** C++20 coroutines, `std::expected`, `std::optional`, `std::span`, `std::array`, `std::shared_ptr`, `galay-kernel` `AwaitableBuilder`, `MachineAction`, `StateMachineAwaitable`, Mongo ring buffer and protocol encoder/decoder.

---

## File structure

- Modify: `galay-mongo/async/async_mongo_client.h`
  - Add `#include <array>`, `#include <coroutine>`, `#include <sys/uio.h>`, and `#include <galay/cpp/galay-kernel/core/awaitable.h>`.
  - Replace the three `Task` aliases with explicit awaitable classes.
  - Keep `AsyncMongoClient` public method signatures unchanged.

- Modify: `galay-mongo/async/async_mongo_client.cc`
  - Keep protocol helpers and `ConnectFlowState`.
  - Add shared I/O helper functions for send windows, recv windows, parse attempts, and common error setting.
  - Implement `MongoConnectAwaitable`.
  - Implement `MongoCommandAwaitable`.
  - Implement `MongoPipelineAwaitable`.
  - Remove coroutine helpers `writevOnce`, `readvOnce`, `connectSocket`, `recvMessage`, and `sendSegments`.
  - Change `AsyncMongoClientInternals` static methods from coroutine implementations to construction helpers or remove them if no longer needed.

- Modify tests only if existing async tests rely on `Task`-specific behavior. Expected existing tests should still compile because call sites use `co_await client.connect(...)`, `co_await client.command(...)`, and `co_await client.pipeline(...)`.

---

### Task 1: Replace Mongo awaitable aliases with explicit class declarations

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.h:4-177`

- [ ] **Step 1: Update includes**

Replace the include block with this shape, preserving existing project includes:

```cpp
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/common/error.h>
#include <galay/cpp/galay-kernel/common/host.hpp>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <coroutine>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <utility>
#include <vector>
```

Expected compile failure after only this step: none.

- [ ] **Step 2: Replace the three `using` aliases**

Delete:

```cpp
using MongoConnectAwaitable = Task<std::expected<bool, MongoError>>;
using MongoCommandAwaitable = Task<std::expected<MongoReply, MongoError>>;
using MongoPipelineAwaitable =
    Task<std::expected<std::vector<MongoPipelineResponse>, MongoError>>;
```

Insert this code at the same location:

```cpp
class MongoConnectAwaitable
{
public:
    using Result = std::expected<bool, MongoError>;

    MongoConnectAwaitable(AsyncMongoClient& client, MongoConfig config);
    MongoConnectAwaitable(MongoConnectAwaitable&&) noexcept = default;
    MongoConnectAwaitable& operator=(MongoConnectAwaitable&&) noexcept = default;
    MongoConnectAwaitable(const MongoConnectAwaitable&) = delete;
    MongoConnectAwaitable& operator=(const MongoConnectAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }

    bool isInvalid() const;

private:
    enum class Phase {
        Invalid,
        Connect,
        SendRequest,
        RecvReply,
        HandleReply,
        Done
    };

    struct SharedState;
    struct Machine;
    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;

    std::shared_ptr<SharedState> m_state;
    InnerAwaitable m_inner;
};

class MongoCommandAwaitable
{
public:
    using Result = std::expected<MongoReply, MongoError>;

    MongoCommandAwaitable(AsyncMongoClient& client,
                          std::string database,
                          MongoDocument command);
    MongoCommandAwaitable(MongoCommandAwaitable&&) noexcept = default;
    MongoCommandAwaitable& operator=(MongoCommandAwaitable&&) noexcept = default;
    MongoCommandAwaitable(const MongoCommandAwaitable&) = delete;
    MongoCommandAwaitable& operator=(const MongoCommandAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }

    bool isInvalid() const;

private:
    enum class Phase {
        Invalid,
        SendCommand,
        RecvReply,
        Done
    };

    struct SharedState;
    struct Machine;
    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;

    std::shared_ptr<SharedState> m_state;
    InnerAwaitable m_inner;
};

class MongoPipelineAwaitable
{
public:
    using Result = std::expected<std::vector<MongoPipelineResponse>, MongoError>;

    MongoPipelineAwaitable(AsyncMongoClient& client,
                           std::string database,
                           std::span<const MongoDocument> commands);
    MongoPipelineAwaitable(MongoPipelineAwaitable&&) noexcept = default;
    MongoPipelineAwaitable& operator=(MongoPipelineAwaitable&&) noexcept = default;
    MongoPipelineAwaitable(const MongoPipelineAwaitable&) = delete;
    MongoPipelineAwaitable& operator=(const MongoPipelineAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }

    bool isInvalid() const;

private:
    enum class Phase {
        Invalid,
        SendCommands,
        RecvReplies,
        Done
    };

    struct SharedState;
    struct Machine;
    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>;

    std::shared_ptr<SharedState> m_state;
    InnerAwaitable m_inner;
};
```

- [ ] **Step 3: Run compile to confirm expected failures**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: FAIL because `MongoConnectAwaitable::SharedState`, `Machine`, and constructors are declared but not implemented.

---

### Task 2: Add shared state-machine helper functions in `async_mongo_client.cc`

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.cc:306-458`

- [ ] **Step 1: Add helper constants and functions after `fillSendIovecsFromSegments`**

Insert the following immediately after `fillSendIovecsFromSegments(...)`:

```cpp
constexpr size_t kMongoMaxWriteIovecs = 3;

size_t totalSegmentLength(std::span<const SendSegment> segments)
{
    size_t total = 0;
    for (const auto& segment : segments) {
        total += segment.len;
    }
    return total;
}

bool prepareWriteWindow(std::array<struct iovec, kMongoMaxWriteIovecs>& write_iovecs,
                        size_t& write_iov_count,
                        std::vector<struct iovec>& scratch,
                        std::span<const SendSegment> segments,
                        size_t sent,
                        std::optional<MongoError>& result_error)
{
    fillSendIovecsFromSegments(scratch, segments, sent);
    if (scratch.empty()) {
        result_error = MongoError(MONGO_ERROR_INTERNAL,
                                  "sendSegments produced empty iovecs");
        write_iov_count = 0;
        return false;
    }
    if (scratch.size() > write_iovecs.size()) {
        result_error = MongoError(MONGO_ERROR_INTERNAL,
                                  "Mongo write iovec count exceeds supported window");
        write_iov_count = 0;
        return false;
    }

    write_iov_count = scratch.size();
    for (size_t i = 0; i < write_iov_count; ++i) {
        write_iovecs[i] = scratch[i];
    }
    return true;
}

bool prepareReadWindow(AsyncMongoClient& client,
                       std::array<struct iovec, 2>& read_iovecs,
                       size_t& read_iov_count,
                       std::string_view no_space_message,
                       std::optional<MongoError>& result_error)
{
    read_iov_count = client.m_ring_buffer.getWriteIovecs(read_iovecs.data(), read_iovecs.size());
    if (read_iov_count == 0) {
        result_error = MongoError(MONGO_ERROR_RECV, std::string(no_space_message));
        return false;
    }
    return true;
}

void applyReadResult(AsyncMongoClient& client,
                     std::expected<size_t, IOError> result,
                     MongoErrorType io_error_type,
                     std::string_view closed_message,
                     std::optional<MongoError>& result_error)
{
    if (!result.has_value()) {
        result_error = mapIoError(result.error(), io_error_type);
        return;
    }
    if (result.value() == 0) {
        result_error = MongoError(MONGO_ERROR_CONNECTION_CLOSED, std::string(closed_message));
        return;
    }
    client.m_ring_buffer.produce(result.value());
}

void applyWriteResult(std::expected<size_t, IOError> result,
                      size_t& sent,
                      MongoErrorType io_error_type,
                      std::string_view closed_message,
                      std::optional<MongoError>& result_error)
{
    if (!result.has_value()) {
        result_error = mapIoError(result.error(), io_error_type);
        return;
    }
    if (result.value() == 0) {
        result_error = MongoError(MONGO_ERROR_CONNECTION_CLOSED, std::string(closed_message));
        return;
    }
    sent += result.value();
}
```

- [ ] **Step 2: Run compile to confirm expected failures remain constructor-related**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: FAIL because awaitable classes are still unimplemented. There should be no new syntax errors from the helper functions.

---

### Task 3: Implement `MongoConnectAwaitable`

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.cc:460-950`

- [ ] **Step 1: Add `MongoConnectAwaitable` state definitions after `AsyncMongoClientInternals` helper state or before client methods**

Add this implementation after `struct AsyncMongoClientInternals`'s `ConnectFlowState` definition and before `int32_t AsyncMongoClient::reserveRequestIdBlock(...)`:

```cpp
struct MongoConnectAwaitable::SharedState {
    SharedState(AsyncMongoClient& client_ref, MongoConfig cfg)
        : client(&client_ref)
        , flow(client_ref, std::move(cfg))
        , host(galay::kernel::IPType::IPV4, flow.config.host, flow.config.port)
    {
        client->m_ring_buffer.clear();
        client->m_decode_scratch.clear();
        auto init_result = flow.initialize();
        if (!init_result.has_value()) {
            result = std::unexpected(std::move(init_result.error()));
            phase = Phase::Invalid;
            return;
        }

        auto nonblock_result = client->m_socket.option().handleNonBlock();
        if (!nonblock_result) {
            result = std::unexpected(MongoError(
                MONGO_ERROR_CONNECTION,
                "Failed to set non-blocking before Mongo connect: " +
                    nonblock_result.error().message()));
            phase = Phase::Invalid;
        }
    }

    AsyncMongoClient* client = nullptr;
    AsyncMongoClientInternals::ConnectFlowState flow;
    galay::kernel::Host host;
    Phase phase = Phase::Connect;
    size_t sent = 0;
    std::array<SendSegment, 1> send_segments{};
    size_t send_segment_count = 0;
    std::vector<struct iovec> write_scratch;
    std::array<struct iovec, kMongoMaxWriteIovecs> write_iovecs{};
    size_t write_iov_count = 0;
    std::array<struct iovec, 2> read_iovecs{};
    size_t read_iov_count = 0;
    std::optional<MongoError> pending_error;
    std::optional<Result> result;
};

struct MongoConnectAwaitable::Machine {
    using result_type = Result;
    static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
        galay::kernel::SequenceOwnerDomain::ReadWrite;

    explicit Machine(std::shared_ptr<SharedState> state)
        : m_state(std::move(state))
    {
    }

    galay::kernel::MachineAction<result_type> advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (m_state->pending_error.has_value()) {
            setError(std::move(*m_state->pending_error));
            m_state->pending_error.reset();
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case Phase::Invalid:
            setError(MongoError(MONGO_ERROR_INTERNAL, "Mongo connect machine entered invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case Phase::Connect:
            return galay::kernel::MachineAction<result_type>::waitConnect(m_state->host);
        case Phase::SendRequest:
            m_state->send_segments[0] = SendSegment{
                m_state->flow.encoded_request.data(),
                m_state->flow.encoded_request.size()
            };
            m_state->send_segment_count = 1;
            if (m_state->sent >= m_state->flow.encoded_request.size()) {
                m_state->sent = 0;
                m_state->phase = Phase::RecvReply;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareWriteWindow(m_state->write_iovecs,
                                    m_state->write_iov_count,
                                    m_state->write_scratch,
                                    std::span<const SendSegment>(m_state->send_segments.data(),
                                                                 m_state->send_segment_count),
                                    m_state->sent,
                                    m_state->pending_error)) {
                setError(std::move(*m_state->pending_error));
                m_state->pending_error.reset();
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitWritev(
                m_state->write_iovecs.data(),
                m_state->write_iov_count);
        case Phase::RecvReply: {
            auto parsed = tryParseMessage(*m_state->client);
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed->has_value()) {
                m_state->current_message = std::move(parsed->value());
                m_state->phase = Phase::HandleReply;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   "No writable ring buffer space while receiving connect/auth reply",
                                   m_state->pending_error)) {
                setError(std::move(*m_state->pending_error));
                m_state->pending_error.reset();
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case Phase::HandleReply:
            return handleReply();
        case Phase::Done:
            if (!m_state->result.has_value()) {
                completeSuccess();
            }
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(MongoError(MONGO_ERROR_INTERNAL, "Unknown Mongo connect machine state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    void onConnect(std::expected<void, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        if (!result.has_value()) {
            setError(mapIoError(result.error(), MONGO_ERROR_CONNECTION));
            return;
        }
        trySetTcpNoDelay(m_state->client->m_socket.handle().fd, m_state->flow.config.tcp_nodelay);
        m_state->phase = Phase::SendRequest;
    }

    void onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        applyReadResult(*m_state->client,
                        result,
                        MONGO_ERROR_RECV,
                        "Connection closed while receiving connect/auth reply",
                        m_state->pending_error);
    }

    void onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        applyWriteResult(result,
                         m_state->sent,
                         MONGO_ERROR_SEND,
                         "Connection closed during connect/auth request send",
                         m_state->pending_error);
        if (!m_state->pending_error.has_value() &&
            m_state->sent >= m_state->flow.encoded_request.size()) {
            m_state->sent = 0;
            m_state->phase = Phase::RecvReply;
        }
    }

private:
    galay::kernel::MachineAction<result_type> handleReply()
    {
        MongoReply reply(std::move(m_state->current_message->body));
        m_state->current_message.reset();
        if (!reply.ok()) {
            setError(makeServerError(std::move(reply), "Mongo connect/auth command failed"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        auto next_result = m_state->flow.handleReply(std::move(reply));
        if (!next_result.has_value()) {
            setError(std::move(next_result.error()));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (next_result.value()) {
            m_state->phase = Phase::Done;
        } else {
            m_state->phase = Phase::SendRequest;
        }
        return galay::kernel::MachineAction<result_type>::continue_();
    }

    void completeSuccess() noexcept
    {
        m_state->client->m_is_closed = false;
        if (m_state->flow.auth_enabled) {
            MongoLogInfo(m_state->client->m_logger.get(),
                         "Mongo connected and authenticated successfully to {}:{}",
                         m_state->flow.config.host,
                         m_state->flow.config.port);
        } else {
            MongoLogInfo(m_state->client->m_logger.get(),
                         "Mongo connected successfully to {}:{}",
                         m_state->flow.config.host,
                         m_state->flow.config.port);
        }
        m_state->result = true;
    }

    void setError(MongoError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = Phase::Invalid;
    }

    std::shared_ptr<SharedState> m_state;
};
```

- [ ] **Step 2: Add the missing current message field**

In `MongoConnectAwaitable::SharedState`, add this member before `pending_error`:

```cpp
std::optional<protocol::MongoMessage> current_message;
```

- [ ] **Step 3: Add constructor and `isInvalid()`**

Add after the `Machine` definition:

```cpp
MongoConnectAwaitable::MongoConnectAwaitable(AsyncMongoClient& client, MongoConfig config)
    : m_state(std::make_shared<SharedState>(client, std::move(config)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MongoConnectAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}
```

- [ ] **Step 4: Run compile and fix only connect-related syntax errors**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: FAIL because command and pipeline awaitables are still unimplemented. Connect-specific errors should be fixed before continuing.

---

### Task 4: Implement `MongoCommandAwaitable`

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.cc`

- [ ] **Step 1: Add command shared state and machine after connect awaitable implementation**

Insert this code after `MongoConnectAwaitable::isInvalid()`:

```cpp
struct MongoCommandAwaitable::SharedState {
    SharedState(AsyncMongoClient& client_ref, std::string db, MongoDocument cmd)
        : client(&client_ref)
        , database(std::move(db))
        , command(std::move(cmd))
    {
        if (client->m_is_closed) {
            result = std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                                "Mongo client is not connected"));
            phase = Phase::Invalid;
            return;
        }

        request_id = client->nextRequestId();
        writeInt32LE(request_id_le.data(), request_id);

        if (isSimplePingCommand(command, database)) {
            if (client->m_ping_encoded_template.empty() || client->m_ping_template_db != database) {
                client->m_ping_template_db = database;
                client->m_ping_encoded_template.clear();
                protocol::MongoProtocol::appendOpMsgWithDatabase(
                    client->m_ping_encoded_template,
                    0,
                    command,
                    database);
            }
            if (client->m_ping_encoded_template.size() < 8) {
                result = std::unexpected(MongoError(MONGO_ERROR_INTERNAL,
                                                    "Invalid cached ping template"));
                phase = Phase::Invalid;
                return;
            }
            send_segments[0] = SendSegment{client->m_ping_encoded_template.data(), 4};
            send_segments[1] = SendSegment{request_id_le.data(), request_id_le.size()};
            send_segments[2] = SendSegment{
                client->m_ping_encoded_template.data() + 8,
                client->m_ping_encoded_template.size() - 8
            };
            send_segment_count = 3;
        } else {
            protocol::MongoProtocol::appendOpMsgWithDatabase(
                encoded_request,
                request_id,
                command,
                database);
            send_segments[0] = SendSegment{encoded_request.data(), encoded_request.size()};
            send_segment_count = 1;
        }
        total_len = totalSegmentLength(std::span<const SendSegment>(send_segments.data(),
                                                                    send_segment_count));
    }

    AsyncMongoClient* client = nullptr;
    std::string database;
    MongoDocument command;
    int32_t request_id = 0;
    std::array<char, 4> request_id_le{};
    std::string encoded_request;
    std::array<SendSegment, 3> send_segments{};
    size_t send_segment_count = 0;
    size_t total_len = 0;
    size_t sent = 0;
    std::vector<struct iovec> write_scratch;
    std::array<struct iovec, kMongoMaxWriteIovecs> write_iovecs{};
    size_t write_iov_count = 0;
    std::array<struct iovec, 2> read_iovecs{};
    size_t read_iov_count = 0;
    Phase phase = Phase::SendCommand;
    std::optional<MongoError> pending_error;
    std::optional<Result> result;
};

struct MongoCommandAwaitable::Machine {
    using result_type = Result;
    static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
        galay::kernel::SequenceOwnerDomain::ReadWrite;

    explicit Machine(std::shared_ptr<SharedState> state)
        : m_state(std::move(state))
    {
    }

    galay::kernel::MachineAction<result_type> advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (m_state->pending_error.has_value()) {
            setError(std::move(*m_state->pending_error));
            m_state->pending_error.reset();
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case Phase::Invalid:
            setError(MongoError(MONGO_ERROR_INTERNAL, "Mongo command machine entered invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case Phase::SendCommand:
            if (m_state->sent >= m_state->total_len) {
                m_state->sent = 0;
                m_state->phase = Phase::RecvReply;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareWriteWindow(m_state->write_iovecs,
                                    m_state->write_iov_count,
                                    m_state->write_scratch,
                                    std::span<const SendSegment>(m_state->send_segments.data(),
                                                                 m_state->send_segment_count),
                                    m_state->sent,
                                    m_state->pending_error)) {
                setError(std::move(*m_state->pending_error));
                m_state->pending_error.reset();
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitWritev(
                m_state->write_iovecs.data(),
                m_state->write_iov_count);
        case Phase::RecvReply: {
            auto parsed = tryParseMessage(*m_state->client);
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (parsed->has_value()) {
                finishWithMessage(std::move(parsed->value()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (!prepareReadWindow(*m_state->client,
                                   m_state->read_iovecs,
                                   m_state->read_iov_count,
                                   "No writable ring buffer space while receiving command reply",
                                   m_state->pending_error)) {
                setError(std::move(*m_state->pending_error));
                m_state->pending_error.reset();
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitReadv(
                m_state->read_iovecs.data(),
                m_state->read_iov_count);
        }
        case Phase::Done:
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(MongoError(MONGO_ERROR_INTERNAL, "Unknown Mongo command machine state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    void onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        applyReadResult(*m_state->client,
                        result,
                        MONGO_ERROR_RECV,
                        "Connection closed while receiving command reply",
                        m_state->pending_error);
    }

    void onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        applyWriteResult(result,
                         m_state->sent,
                         MONGO_ERROR_SEND,
                         "Connection closed during command send",
                         m_state->pending_error);
        if (!m_state->pending_error.has_value() && m_state->sent >= m_state->total_len) {
            m_state->sent = 0;
            m_state->phase = Phase::RecvReply;
        }
    }

private:
    void finishWithMessage(protocol::MongoMessage message)
    {
        if (message.header.response_to != m_state->request_id) {
            setError(MongoError(MONGO_ERROR_PROTOCOL,
                                "Response responseTo does not match sent requestId"));
            return;
        }

        MongoReply reply(std::move(message.body));
        if (!reply.ok()) {
            setError(makeServerError(std::move(reply), "Mongo command failed"));
            return;
        }

        m_state->phase = Phase::Done;
        m_state->result = std::move(reply);
    }

    void setError(MongoError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = Phase::Invalid;
    }

    std::shared_ptr<SharedState> m_state;
};

MongoCommandAwaitable::MongoCommandAwaitable(AsyncMongoClient& client,
                                             std::string database,
                                             MongoDocument command)
    : m_state(std::make_shared<SharedState>(client, std::move(database), std::move(command)))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MongoCommandAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}
```

- [ ] **Step 2: Run compile and fix only command-related syntax errors**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: FAIL because pipeline awaitable is still unimplemented. Command-specific errors should be fixed before continuing.

---

### Task 5: Implement `MongoPipelineAwaitable`

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.cc`

- [ ] **Step 1: Add pipeline shared state and machine after command awaitable implementation**

Insert this code after `MongoCommandAwaitable::isInvalid()`:

```cpp
struct MongoPipelineAwaitable::SharedState {
    SharedState(AsyncMongoClient& client_ref,
                std::string db,
                std::span<const MongoDocument> commands)
        : client(&client_ref)
        , database(std::move(db))
    {
        if (client->m_is_closed) {
            result = std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                                "Mongo client is not connected"));
            phase = Phase::Invalid;
            return;
        }
        if (commands.empty()) {
            result = std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM,
                                                "Pipeline commands must not be empty"));
            phase = Phase::Invalid;
            return;
        }
        if (commands.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            result = std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM,
                                                "Pipeline commands exceed supported size"));
            phase = Phase::Invalid;
            return;
        }

        responses.resize(commands.size());
        first_request_id = client->reserveRequestIdBlock(commands.size());
        for (size_t i = 0; i < commands.size(); ++i) {
            responses[i].request_id = static_cast<int32_t>(
                static_cast<int64_t>(first_request_id) + static_cast<int64_t>(i));
        }

        encoded_batch = protocol::MongoCommandBuilder::encodePipeline(
            database,
            first_request_id,
            commands,
            client->m_pipeline_reserve_per_command);
        send_segments[0] = SendSegment{encoded_batch.data(), encoded_batch.size()};
        send_segment_count = 1;
        total_len = encoded_batch.size();
    }

    AsyncMongoClient* client = nullptr;
    std::string database;
    int32_t first_request_id = 0;
    std::string encoded_batch;
    std::array<SendSegment, 1> send_segments{};
    size_t send_segment_count = 0;
    size_t total_len = 0;
    size_t sent = 0;
    size_t received = 0;
    std::vector<MongoPipelineResponse> responses;
    std::vector<struct iovec> write_scratch;
    std::array<struct iovec, kMongoMaxWriteIovecs> write_iovecs{};
    size_t write_iov_count = 0;
    std::array<struct iovec, 2> read_iovecs{};
    size_t read_iov_count = 0;
    Phase phase = Phase::SendCommands;
    std::optional<MongoError> pending_error;
    std::optional<Result> result;
};

struct MongoPipelineAwaitable::Machine {
    using result_type = Result;
    static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
        galay::kernel::SequenceOwnerDomain::ReadWrite;

    explicit Machine(std::shared_ptr<SharedState> state)
        : m_state(std::move(state))
    {
    }

    galay::kernel::MachineAction<result_type> advance()
    {
        if (m_state->result.has_value()) {
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }
        if (m_state->pending_error.has_value()) {
            setError(std::move(*m_state->pending_error));
            m_state->pending_error.reset();
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        switch (m_state->phase) {
        case Phase::Invalid:
            setError(MongoError(MONGO_ERROR_INTERNAL, "Mongo pipeline machine entered invalid state"));
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        case Phase::SendCommands:
            if (m_state->sent >= m_state->total_len) {
                m_state->sent = 0;
                m_state->phase = Phase::RecvReplies;
                return galay::kernel::MachineAction<result_type>::continue_();
            }
            if (!prepareWriteWindow(m_state->write_iovecs,
                                    m_state->write_iov_count,
                                    m_state->write_scratch,
                                    std::span<const SendSegment>(m_state->send_segments.data(),
                                                                 m_state->send_segment_count),
                                    m_state->sent,
                                    m_state->pending_error)) {
                setError(std::move(*m_state->pending_error));
                m_state->pending_error.reset();
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            return galay::kernel::MachineAction<result_type>::waitWritev(
                m_state->write_iovecs.data(),
                m_state->write_iov_count);
        case Phase::RecvReplies:
            return receiveReplies();
        case Phase::Done:
            return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
        }

        setError(MongoError(MONGO_ERROR_INTERNAL, "Unknown Mongo pipeline machine state"));
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    void onRead(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        applyReadResult(*m_state->client,
                        result,
                        MONGO_ERROR_RECV,
                        "Connection closed while receiving pipeline replies",
                        m_state->pending_error);
    }

    void onWrite(std::expected<size_t, IOError> result)
    {
        if (m_state->result.has_value()) {
            return;
        }
        applyWriteResult(result,
                         m_state->sent,
                         MONGO_ERROR_SEND,
                         "Connection closed during pipeline send",
                         m_state->pending_error);
        if (!m_state->pending_error.has_value() && m_state->sent >= m_state->total_len) {
            m_state->sent = 0;
            m_state->phase = Phase::RecvReplies;
        }
    }

private:
    galay::kernel::MachineAction<result_type> receiveReplies()
    {
        while (m_state->received < m_state->responses.size()) {
            auto parsed = tryParseMessage(*m_state->client);
            if (!parsed.has_value()) {
                setError(std::move(parsed.error()));
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            if (!parsed->has_value()) {
                if (!prepareReadWindow(*m_state->client,
                                       m_state->read_iovecs,
                                       m_state->read_iov_count,
                                       "No writable ring buffer space while receiving pipeline replies",
                                       m_state->pending_error)) {
                    setError(std::move(*m_state->pending_error));
                    m_state->pending_error.reset();
                    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
                }
                return galay::kernel::MachineAction<result_type>::waitReadv(
                    m_state->read_iovecs.data(),
                    m_state->read_iov_count);
            }

            if (!storeResponse(std::move(parsed->value()))) {
                return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
            }
            ++m_state->received;
        }

        m_state->phase = Phase::Done;
        m_state->result = std::move(m_state->responses);
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    bool storeResponse(protocol::MongoMessage message)
    {
        const int32_t response_to = message.header.response_to;
        if (response_to <= 0) {
            setError(MongoError(MONGO_ERROR_PROTOCOL,
                                "Pipeline response has invalid responseTo"));
            return false;
        }

        const int64_t index_i64 =
            static_cast<int64_t>(response_to) - static_cast<int64_t>(m_state->first_request_id);
        if (index_i64 < 0 || index_i64 >= static_cast<int64_t>(m_state->responses.size())) {
            setError(MongoError(MONGO_ERROR_PROTOCOL,
                                "Pipeline responseTo does not match any in-flight requestId"));
            return false;
        }

        auto& slot = m_state->responses[static_cast<size_t>(index_i64)];
        if (slot.request_id != response_to) {
            setError(MongoError(MONGO_ERROR_PROTOCOL,
                                "Pipeline responseTo does not map to expected requestId"));
            return false;
        }
        if (slot.reply.has_value() || slot.error.has_value()) {
            setError(MongoError(MONGO_ERROR_PROTOCOL,
                                "Pipeline received duplicate response for the same requestId"));
            return false;
        }

        MongoReply reply(std::move(message.body));
        if (reply.ok()) {
            slot.reply = std::move(reply);
        } else {
            slot.error = makeServerError(std::move(reply), "Mongo pipeline command failed");
        }
        return true;
    }

    void setError(MongoError error) noexcept
    {
        m_state->result = std::unexpected(std::move(error));
        m_state->phase = Phase::Invalid;
    }

    std::shared_ptr<SharedState> m_state;
};

MongoPipelineAwaitable::MongoPipelineAwaitable(AsyncMongoClient& client,
                                               std::string database,
                                               std::span<const MongoDocument> commands)
    : m_state(std::make_shared<SharedState>(client, std::move(database), commands))
    , m_inner(galay::kernel::AwaitableBuilder<Result>::fromStateMachine(
                  client.socket().controller(),
                  Machine(m_state))
                  .build())
{
}

bool MongoPipelineAwaitable::isInvalid() const
{
    return m_state != nullptr && m_state->phase == Phase::Invalid;
}
```

- [ ] **Step 2: Run compile and fix pipeline-specific syntax errors**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: FAIL only because old coroutine internals still conflict or because helper placement needs adjustment. Fix pipeline-specific errors before continuing.

---

### Task 6: Remove old coroutine I/O helpers and wire client methods to awaitable constructors

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.cc:736-1243`

- [ ] **Step 1: Delete obsolete coroutine helpers**

Delete these functions from `struct AsyncMongoClientInternals`:

```cpp
static Task<std::expected<size_t, IOError>> writevOnce(...);
static Task<std::expected<size_t, IOError>> readvOnce(...);
static Task<std::expected<void, MongoError>> connectSocket(...);
static Task<std::expected<protocol::MongoMessage, MongoError>> recvMessage(...);
static Task<std::expected<bool, MongoError>> sendSegments(...);
static MongoConnectAwaitable connect(...);
static MongoCommandAwaitable command(...);
static MongoPipelineAwaitable pipeline(...);
```

Keep `ConnectFlowState` and any helper logic used by the new machines.

- [ ] **Step 2: Replace `AsyncMongoClient` method bodies**

Ensure these method bodies are exactly construction-only:

```cpp
MongoConnectAwaitable AsyncMongoClient::connect(MongoConfig config)
{
    return MongoConnectAwaitable(*this, std::move(config));
}

MongoConnectAwaitable AsyncMongoClient::connect(std::string_view host,
                                                uint16_t port,
                                                std::string_view database)
{
    MongoConfig config;
    config.host.assign(host.data(), host.size());
    config.port = port;
    config.database.assign(database.data(), database.size());
    return connect(std::move(config));
}

MongoCommandAwaitable AsyncMongoClient::command(std::string database, MongoDocument command)
{
    return MongoCommandAwaitable(*this, std::move(database), std::move(command));
}

MongoCommandAwaitable AsyncMongoClient::ping(std::string database)
{
    MongoDocument ping_doc;
    ping_doc.append("ping", int32_t(1));
    return command(std::move(database), std::move(ping_doc));
}

MongoPipelineAwaitable AsyncMongoClient::pipeline(std::string database,
                                                  std::span<const MongoDocument> commands)
{
    return MongoPipelineAwaitable(*this, std::move(database), commands);
}
```

- [ ] **Step 3: Run compile**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: PASS or only concrete API mismatch errors with `MachineAction::waitWritev` / `waitConnect`. If `waitWritev` expects a different pointer/count signature, adjust to match the Redis/HTTP usage already present in the local repositories.

---

### Task 7: Add timeout support only if required by current kernel patterns

**Files:**
- Modify: `galay-mongo/async/async_mongo_client.h`
- Modify: `galay-mongo/async/async_mongo_client.cc`

- [ ] **Step 1: Inspect compile/runtime behavior before changing timeout semantics**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: PASS before changing timeout support. If it fails because state-machine awaitables require a `markTimeout()` method or `TimeoutSupport` inheritance, apply Step 2.

- [ ] **Step 2: If required, add `TimeoutSupport` inheritance matching MySQL query awaitables**

In `async_mongo_client.h`, change each class declaration to inherit timeout support:

```cpp
class MongoConnectAwaitable
    : public galay::kernel::TimeoutSupport<MongoConnectAwaitable>
```

```cpp
class MongoCommandAwaitable
    : public galay::kernel::TimeoutSupport<MongoCommandAwaitable>
```

```cpp
class MongoPipelineAwaitable
    : public galay::kernel::TimeoutSupport<MongoPipelineAwaitable>
```

Add this method to each public section:

```cpp
void markTimeout() { m_inner.markTimeout(); }
```

Add include if missing:

```cpp
#include <galay/cpp/galay-kernel/core/timeout.hpp>
```

- [ ] **Step 3: Keep timeout error mapping unchanged**

Do not add a new timeout error path. Existing I/O errors must still pass through:

```cpp
mapIoError(result.error(), MONGO_ERROR_RECV)
mapIoError(result.error(), MONGO_ERROR_SEND)
mapIoError(result.error(), MONGO_ERROR_CONNECTION)
```

Expected: `IOError::contains(..., kTimeout)` still maps to `MONGO_ERROR_TIMEOUT`.

---

### Task 8: Build and run existing Mongo tests

**Files:**
- No code changes unless tests expose a regression.

- [ ] **Step 1: Format changed files**

Run:

```bash
rtk clang-format -i /Users/gongzhijie/Desktop/projects/git/galay-mongo/galay-mongo/async/async_mongo_client.h /Users/gongzhijie/Desktop/projects/git/galay-mongo/galay-mongo/async/async_mongo_client.cc
```

Expected: command succeeds with no output or compact RTK output.

- [ ] **Step 2: Build project**

Run:

```bash
rtk cmake --build /Users/gongzhijie/Desktop/projects/git/galay-mongo/build
```

Expected: PASS.

- [ ] **Step 3: Run protocol and bridge tests that should not require a live Mongo server**

Run:

```bash
rtk ctest --test-dir /Users/gongzhijie/Desktop/projects/git/galay-mongo/build -R "T7-sync_large_message_bridge|T8-protocol_builder" --output-on-failure
```

Expected: PASS.

- [ ] **Step 4: Run async Mongo tests when the local Mongo test service is available**

Run:

```bash
rtk ctest --test-dir /Users/gongzhijie/Desktop/projects/git/galay-mongo/build -R "T3-async_mongo_pipeline|T5-async_mongo_functional|T6-auth_compatibility" --output-on-failure
```

Expected with Mongo service available: PASS. If the service is unavailable, record the connection failure and run the non-network tests from Step 3.

- [ ] **Step 5: Inspect diff**

Run:

```bash
rtk git -C /Users/gongzhijie/Desktop/projects/git/galay-mongo diff -- galay-mongo/async/async_mongo_client.h galay-mongo/async/async_mongo_client.cc docs/superpowers/specs/2026-04-27-mongo-state-machine-design.md docs/superpowers/plans/2026-04-27-mongo-state-machine-awaitables.md
```

Expected: diff only contains the state-machine refactor and plan/spec docs.

---

## Self-review checklist

- Spec coverage:
  - Explicit awaitable classes: Task 1.
  - Connect state machine aligned with HTTP/MySQL: Task 3.
  - Command state machine: Task 4.
  - Pipeline state machine: Task 5.
  - Reused Mongo protocol logic: Tasks 2-6.
  - Error semantics: Tasks 2-6.
  - Timeout behavior: Task 7.
  - Build and tests: Task 8.

- Placeholder scan: no `TBD`, no `TODO`, no omitted error handling, no unspecified file paths.

- Type consistency:
  - All public results match the approved design.
  - All machines use `galay::kernel::StateMachineAwaitable<Machine>`.
  - All I/O paths use `MachineAction` through `AwaitableBuilder<Result>::fromStateMachine(...)`.
