#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/client.h>
#include <galay/cpp/galay-postgres/protoc/postgres_protocol.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace galay::kernel;
using namespace galay::postgres;
using namespace std::chrono_literals;

namespace
{

constexpr size_t kLargeQueryBytes = 512 * 1024;

bool readExact(int fd, char* output, size_t length)
{
    size_t received = 0;
    while (received < length) {
        const ssize_t count = ::recv(fd, output + received, length - received, 0);
        if (count <= 0) {
            return false;
        }
        received += static_cast<size_t>(count);
    }
    return true;
}

bool readFrontendMessage(int fd, char expected_type, std::string* payload)
{
    std::array<char, 5> header{};
    if (!readExact(fd, header.data(), header.size()) || header[0] != expected_type) {
        return false;
    }
    const uint32_t length = protocol::readInt32(header.data() + 1);
    if (length < 4 || length > 16 * 1024 * 1024) {
        return false;
    }
    payload->resize(length - 4);
    return readExact(fd, payload->data(), payload->size());
}

bool isQueryPayload(std::string_view payload, std::string_view sql)
{
    return payload.size() == sql.size() + 1 && payload.back() == '\0' &&
           payload.substr(0, sql.size()) == sql;
}

bool readStartupMessage(int fd)
{
    std::array<char, 4> header{};
    if (!readExact(fd, header.data(), header.size())) {
        return false;
    }
    const uint32_t length = protocol::readInt32(header.data());
    if (length < 8 || length > protocol::kMaxStartupPacketLength) {
        return false;
    }
    std::string payload(length - 4, '\0');
    return readExact(fd, payload.data(), payload.size()) &&
           protocol::readInt32(payload.data()) == protocol::kProtocolVersion3;
}

std::string backendMessage(char type, std::string_view payload)
{
    std::string message;
    message.reserve(5 + payload.size());
    message.push_back(type);
    protocol::writeInt32(message, static_cast<uint32_t>(4 + payload.size()));
    message.append(payload);
    return message;
}

bool sendFragmented(int fd, std::string_view bytes)
{
    constexpr std::array<size_t, 5> kChunks{1, 2, 3, 1, 4};
    size_t sent = 0;
    size_t chunk_index = 0;
    while (sent < bytes.size()) {
        const size_t requested = std::min(kChunks[chunk_index++ % kChunks.size()],
                                          bytes.size() - sent);
        const ssize_t count = ::send(fd, bytes.data() + sent, requested, MSG_NOSIGNAL);
        if (count <= 0) {
            return false;
        }
        sent += static_cast<size_t>(count);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

std::string rowDescription()
{
    std::string payload;
    protocol::writeInt16(payload, 1);
    protocol::writeCString(payload, "value");
    protocol::writeInt32(payload, 0);
    protocol::writeInt16(payload, 0);
    protocol::writeInt32(payload, static_cast<uint32_t>(PostgresOid::INT4));
    protocol::writeInt16(payload, 4);
    protocol::writeInt32(payload, UINT32_MAX);
    protocol::writeInt16(payload, 0);
    return backendMessage(protocol::kMsgRowDescription, payload);
}

std::string dataRow(std::string_view value)
{
    std::string payload;
    protocol::writeInt16(payload, 1);
    protocol::writeInt32(payload, static_cast<uint32_t>(value.size()));
    payload.append(value);
    return backendMessage(protocol::kMsgDataRow, payload);
}

std::string successfulQueryResponse(std::string_view value)
{
    std::string response = rowDescription();
    response += dataRow(value);
    response += backendMessage(protocol::kMsgCommandComplete,
                               std::string_view("SELECT 1\0", 9));
    response += backendMessage(protocol::kMsgReadyForQuery, "I");
    return response;
}

std::string startupResponse()
{
    std::string auth_payload;
    protocol::writeInt32(auth_payload, 0);
    std::string response = backendMessage(protocol::kMsgAuthentication, auth_payload);

    std::string parameter_payload;
    protocol::writeCString(parameter_payload, "server_version");
    protocol::writeCString(parameter_payload, "16.0");
    response += backendMessage(protocol::kMsgParameterStatus, parameter_payload);

    std::string key_payload;
    protocol::writeInt32(key_payload, 123);
    protocol::writeInt32(key_payload, 456);
    response += backendMessage(protocol::kMsgBackendKeyData, key_payload);
    response += backendMessage(protocol::kMsgReadyForQuery, "I");
    return response;
}

std::string failedQueryResponse()
{
    std::string error_payload;
    error_payload.push_back('S');
    protocol::writeCString(error_payload, "ERROR");
    error_payload.push_back('C');
    protocol::writeCString(error_payload, "42601");
    error_payload.push_back('M');
    protocol::writeCString(error_payload, "mock syntax error");
    error_payload.push_back('\0');

    std::string response = backendMessage(protocol::kMsgErrorResponse, error_payload);
    response += backendMessage(protocol::kMsgReadyForQuery, "I");
    return response;
}

class MockPostgresServer
{
public:
    MockPostgresServer()
    {
        m_listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listener < 0) {
            m_error = "socket failed";
            return;
        }
        const int enabled = 1;
        if (::setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
            m_error = "setsockopt(SO_REUSEADDR) failed";
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
            ::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(m_listener, 4) != 0) {
            m_error = "bind/listen failed";
            return;
        }
        socklen_t address_length = sizeof(address);
        if (::getsockname(m_listener,
                          reinterpret_cast<sockaddr*>(&address),
                          &address_length) != 0) {
            m_error = "getsockname failed";
            return;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this] { run(); });
    }

    ~MockPostgresServer()
    {
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_listener >= 0) {
            (void)::close(m_listener);
        }
    }

    MockPostgresServer(const MockPostgresServer&) = delete;
    MockPostgresServer& operator=(const MockPostgresServer&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_port != 0 && m_error.empty(); }
    [[nodiscard]] uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
    void fail(std::string message)
    {
        if (m_error.empty()) {
            m_error = std::move(message);
        }
    }

    void run()
    {
        const int connection = ::accept(m_listener, nullptr, nullptr);
        if (connection < 0) {
            fail("accept failed");
            return;
        }
        timeval timeout{.tv_sec = 5, .tv_usec = 0};
        (void)::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string payload;
        if (!readStartupMessage(connection) ||
            !sendFragmented(connection, startupResponse())) {
            fail("startup exchange failed");
            (void)::close(connection);
            return;
        }

        std::this_thread::sleep_for(50ms);
        if (!readFrontendMessage(connection, protocol::kMsgQuery, &payload) ||
            payload.size() < kLargeQueryBytes ||
            !sendFragmented(connection, successfulQueryResponse("1"))) {
            fail("large-query exchange failed");
            (void)::close(connection);
            return;
        }

        if (!readFrontendMessage(connection, protocol::kMsgQuery, &payload) ||
            !sendFragmented(connection, failedQueryResponse())) {
            fail("failed-query exchange failed");
            (void)::close(connection);
            return;
        }

        if (!readFrontendMessage(connection, protocol::kMsgQuery, &payload) ||
            !sendFragmented(connection, successfulQueryResponse("2"))) {
            fail("recovery-query exchange failed");
            (void)::close(connection);
            return;
        }

        if (!readFrontendMessage(connection, protocol::kMsgQuery, &payload) ||
            !isQueryPayload(payload, "SELECT held")) {
            fail("single-operation held query failed");
            (void)::close(connection);
            return;
        }
        std::this_thread::sleep_for(100ms);
        if (!sendFragmented(connection, successfulQueryResponse("3"))) {
            fail("single-operation held response failed");
            (void)::close(connection);
            return;
        }

        if (!readFrontendMessage(connection, protocol::kMsgQuery, &payload) ||
            !isQueryPayload(payload, "SELECT 4") ||
            !sendFragmented(connection, successfulQueryResponse("4"))) {
            fail("post-conflict recovery query failed");
            (void)::close(connection);
            return;
        }

        if (!readFrontendMessage(connection, protocol::kMsgQuery, &payload) ||
            !isQueryPayload(payload, "SELECT malformed")) {
            fail("malformed-response query failed");
            (void)::close(connection);
            return;
        }
        std::string malformed = "T";
        protocol::writeInt32(malformed, 3);
        if (!sendFragmented(connection, malformed)) {
            fail("malformed response send failed");
        }
        (void)::close(connection);
    }

    std::thread m_thread;
    std::string m_error;
    int m_listener = -1;
    uint16_t m_port = 0;
};

using AsyncQueryResult = PostgresQueryAwaitable<>::Result;

struct HeldQueryState
{
    AsyncWaiter<AsyncQueryResult> completed;
    std::atomic<bool> notified{false};
};

Task<void> runHeldQuery(AsyncPostgresClient<>* client, HeldQueryState* state)
{
    auto result = co_await client->query("SELECT held").timeout(3s);
    state->notified.store(state->completed.notify(std::move(result)),
                          std::memory_order_release);
}

Task<int> runClient(IOScheduler* scheduler, uint16_t port)
{
    AsyncPostgresConfig async_config;
    async_config.buffer_size = 256;
    AsyncPostgresClient<> client(scheduler, async_config);

    PostgresConfig config = PostgresConfig::create("127.0.0.1", port, "mock", "", "mock");
    auto connected = co_await client.connect(std::move(config)).timeout(3s);
    if (!connected || !connected->has_value() || !**connected || client.isClosed()) {
        co_return 1;
    }
    const int send_buffer = 64 * 1024;
    if (::setsockopt(client.socket().handle().fd,
                     SOL_SOCKET,
                     SO_SNDBUF,
                     &send_buffer,
                     sizeof(send_buffer)) != 0) {
        co_return 6;
    }
    const auto parameter = client.serverParameters().find("server_version");
    if (parameter == client.serverParameters().end() || parameter->second != "16.0" ||
        !client.backendKeyData().has_value()) {
        co_return 2;
    }

    std::vector<uint32_t> too_many_types(32768);
    auto invalid_prepare = co_await client.prepare(
        "oversized", "SELECT 1", too_many_types).timeout(1s);
    if (invalid_prepare || invalid_prepare.error().type() != POSTGRES_ERROR_INVALID_PARAM ||
        client.isClosed()) {
        co_return 11;
    }

    std::vector<std::optional<std::string_view>> too_many_parameters(32768);
    auto invalid_execute = co_await client.execute(
        "oversized", too_many_parameters).timeout(1s);
    if (invalid_execute || invalid_execute.error().type() != POSTGRES_ERROR_INVALID_PARAM ||
        client.isClosed()) {
        co_return 12;
    }

    std::string large_sql = "SELECT 1 /*";
    large_sql.append(kLargeQueryBytes, 'x');
    large_sql += "*/";
    auto first = co_await client.query(large_sql).timeout(5s);
    if (!first || !first->has_value() || first->value().rowCount() != 1 ||
        first->value().row(0).getInt64(0, -1) != 1) {
        co_return 3;
    }

    auto failed = co_await client.query("SELECT broken").timeout(3s);
    if (failed || failed.error().sqlState() != "42601" ||
        client.transactionStatus() != 'I') {
        co_return 4;
    }

    auto recovered = co_await client.query("SELECT 2").timeout(3s);
    if (!recovered || !recovered->has_value() || recovered->value().rowCount() != 1 ||
        recovered->value().row(0).getInt64(0, -1) != 2) {
        co_return 5;
    }

    HeldQueryState held_query;
    if (!scheduleTask(scheduler, runHeldQuery(&client, &held_query))) {
        co_return 7;
    }
    co_yield true;

    auto overlapping = co_await client.query("SELECT overlap").timeout(1s);
    if (overlapping || overlapping.error().type() != POSTGRES_ERROR_INTERNAL ||
        client.isClosed()) {
        co_return 8;
    }

    auto reconnecting = co_await client.connect(
        PostgresConfig::create("127.0.0.1", port, "mock", "", "mock")).timeout(1s);
    if (reconnecting || reconnecting.error().type() != POSTGRES_ERROR_INTERNAL ||
        client.isClosed()) {
        co_return 13;
    }

    auto closing = co_await client.close();
    if (!closing || closing->has_value() ||
        closing->error().type() != POSTGRES_ERROR_INTERNAL || client.isClosed()) {
        co_return 15;
    }

    auto held = co_await held_query.completed.wait().timeout(3s);
    if (!held || !held->has_value() || !held->value().has_value() ||
        held->value().value().rowCount() != 1 ||
        held->value().value().row(0).getInt64(0, -1) != 3 ||
        !held_query.notified.load(std::memory_order_acquire)) {
        co_return 9;
    }

    auto final_query = co_await client.query("SELECT 4").timeout(3s);
    if (!final_query || !final_query->has_value() ||
        final_query->value().rowCount() != 1 ||
        final_query->value().row(0).getInt64(0, -1) != 4) {
        co_return 10;
    }

    auto malformed = co_await client.query("SELECT malformed").timeout(3s);
    if (malformed || malformed.error().type() != POSTGRES_ERROR_PROTOCOL ||
        !client.isClosed()) {
        co_return 14;
    }
    co_return 0;
}

} // namespace

int main()
{
    MockPostgresServer server;
    if (!server.valid()) {
        std::cerr << "mock PostgreSQL server setup failed: " << server.error() << '\n';
        return EXIT_FAILURE;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(0).build();
    auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed\n";
        return EXIT_FAILURE;
    }
    IOScheduler* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "missing IO scheduler\n";
        return EXIT_FAILURE;
    }

    auto result = runtime.blockOnIO(runClient(scheduler, server.port()));
    runtime.stop();
    if (!result || *result != 0) {
        std::cerr << "async PostgreSQL boundary client failed at step "
                  << (result ? *result : -1) << '\n';
        return EXIT_FAILURE;
    }
    if (!server.error().empty()) {
        std::cerr << "mock PostgreSQL server failed: " << server.error() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
