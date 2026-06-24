# Mongo async state machine refactor design

## Goal

Unify `galay-mongo` async protocol awaitables with the state-machine style already used by `galay-http`, `galay-redis`, and `galay-mysql`.

The public API behavior stays unchanged:

- `connect` returns `std::expected<bool, MongoError>`.
- `command` returns `std::expected<MongoReply, MongoError>`.
- `pipeline` returns `std::expected<std::vector<MongoPipelineResponse>, MongoError>`.

The implementation changes from coroutine `Task` helpers to explicit state-machine awaitables.

## Architecture

Replace the three `Task` aliases in `async_mongo_client.h` with value awaitable classes:

- `MongoConnectAwaitable`
- `MongoCommandAwaitable`
- `MongoPipelineAwaitable`

Each awaitable owns:

- `std::shared_ptr<SharedState> m_state`
- `galay::kernel::StateMachineAwaitable<Machine> m_inner`

Each machine exposes:

- `advance()`
- `onRead(std::expected<size_t, IOError>)`
- `onWrite(std::expected<size_t, IOError>)`
- `onConnect(std::expected<void, IOError>)` for connect only

All machines use `SequenceOwnerDomain::ReadWrite`.

`AsyncMongoClient::connect`, `command`, `ping`, and `pipeline` only construct and return these awaitables. They no longer return coroutine `Task` objects.

## State flows

### Connect

`MongoConnectAwaitable` follows the same state-machine model as `galay-http` connection handling and the MySQL connect awaitable.

Phases:

1. `Connect`
2. `SendRequest`
3. `RecvReply`
4. `HandleReply`
5. `Done`

The machine performs socket connect first, then sends the initial hello request. If authentication is configured, it continues through the existing SCRAM steps until `ConnectFlowState::handleReply` reports completion.

### Command

Phases:

1. `Invalid`
2. `SendCommand`
3. `RecvReply`
4. `Done`

The awaitable encodes the command once in `SharedState`. Ping keeps the existing cached template optimization. Writes support partial completion by tracking sent bytes. Reads parse incrementally from the ring buffer.

### Pipeline

Phases:

1. `Invalid`
2. `SendCommands`
3. `RecvReplies`
4. `Done`

The awaitable encodes the command batch once, sends it with partial-write support, then receives replies until every expected request id has a response. Existing response matching rules are preserved.

## Reused logic

Keep existing protocol and parsing logic where possible:

- `ConnectFlowState`
- SCRAM helper functions
- `prepareDecodeView`
- `tryParseMessage`
- `fillSendIovecsFromSegments`
- request id allocation
- ping template cache
- pipeline response validation
- server reply error mapping

Remove coroutine I/O helpers after their behavior is moved into machines:

- `readvOnce`
- `writevOnce`
- `connectSocket`
- `recvMessage`
- `sendSegments`

## I/O behavior

Write path:

- Build iovecs from the current encoded request or send segments.
- Return `MachineAction::waitWritev(...)`.
- `onWrite()` maps errors, treats zero-byte writes as connection closed, and advances the sent-byte cursor.

Read path:

- `advance()` first tries to parse a complete Mongo message from the ring buffer.
- If parsing is incomplete, prepare writable ring-buffer iovecs.
- Return `MachineAction::waitReadv(...)`.
- `onRead()` maps errors, treats zero-byte reads as connection closed, and calls `ring_buffer.produce(bytes)`.

Connect path:

- Set the socket to nonblocking before `waitConnect`.
- On successful connect, apply TCP options such as `tcp_nodelay`.
- Then transition to `SendRequest`.

## Error handling

Preserve existing error semantics:

- I/O errors use `mapIoError(...)`.
- Timeout I/O errors map to `MONGO_ERROR_TIMEOUT`.
- Disconnect I/O errors map to `MONGO_ERROR_CONNECTION_CLOSED`.
- Zero-byte read/write maps to `MONGO_ERROR_CONNECTION_CLOSED` with the current operation-specific message.
- Ring buffer exhaustion while receiving maps to `MONGO_ERROR_RECV` with the existing message.
- Protocol violations keep current `MONGO_ERROR_PROTOCOL` messages.
- Server replies with `ok == false` keep using `makeServerError(...)`.

## Timeout behavior

Do not change public timeout configuration.

`AsyncMongoConfig::send_timeout` and `recv_timeout` remain the source of timeout settings. The implementation should follow the existing kernel state-machine timeout pattern used by Redis/MySQL. If the kernel only supports state-machine-level timeout, use that established style instead of introducing a new timeout mechanism.

The required behavior is that send/read timeout failures still surface as `MONGO_ERROR_TIMEOUT`.

## Testing

Update or add async Mongo tests for:

- connect state-machine success path
- command/ping success path
- pipeline multi-response matching
- `responseTo` mismatch
- server error reply
- ring buffer space exhaustion
- peer close during send/receive

Run the existing Mongo test targets, especially:

- `T3-async_mongo_pipeline`
- `T5-async_mongo_functional`
- `T6-auth_compatibility`
- `T7-sync_large_message_bridge`
- `T8-protocol_builder`

Run the project build and format changed C++ files with clang-format before completion.
