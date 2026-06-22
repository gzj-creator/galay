#include "client.h"

#include <galay/cpp/galay-mongo/protoc/builder.h>

#include <galay/cpp/galay-kernel/common/handle_option.h>
#include <galay/cpp/galay-utils/crypto/hmac.hpp>
#include <galay/cpp/galay-utils/crypto/pbkdf2.hpp>
#include <galay/cpp/galay-utils/crypto/salt.hpp>
#include <galay/cpp/galay-utils/encoding/base64.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace galay::mongo
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

void writeInt32LE(char* p, int32_t value)
{
    const auto u = static_cast<uint32_t>(value);
    p[0] = static_cast<char>(u & 0xFF);
    p[1] = static_cast<char>((u >> 8) & 0xFF);
    p[2] = static_cast<char>((u >> 16) & 0xFF);
    p[3] = static_cast<char>((u >> 24) & 0xFF);
}

MongoError mapIoError(const IOError& io_error, MongoErrorType fallback)
{
    if (IOError::contains(io_error.code(), galay::kernel::kTimeout)) {
        return MongoError(MONGO_ERROR_TIMEOUT, io_error.message());
    }
    if (IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
        return MongoError(MONGO_ERROR_CONNECTION_CLOSED, io_error.message());
    }
    return MongoError(fallback, io_error.message());
}

MongoError mapTaskResultError(const galay::kernel::detail::TaskResultError& task_error,
                              MongoErrorType fallback)
{
    return MongoError(fallback, std::string(task_error.message()));
}

std::expected<size_t, MongoError>
flattenIoTaskResult(std::expected<std::expected<size_t, IOError>,
                                  galay::kernel::detail::TaskResultError>&& task_result,
                    MongoErrorType fallback)
{
    if (!task_result) {
        return std::unexpected(mapTaskResultError(task_result.error(), fallback));
    }

    auto io_result = std::move(task_result.value());
    if (!io_result) {
        return std::unexpected(mapIoError(io_result.error(), fallback));
    }
    return io_result.value();
}

template <typename T>
std::expected<T, MongoError>
flattenMongoTaskResult(std::expected<std::expected<T, MongoError>,
                                     galay::kernel::detail::TaskResultError>&& task_result,
                       MongoErrorType fallback)
{
    if (!task_result) {
        return std::unexpected(mapTaskResultError(task_result.error(), fallback));
    }
    return std::move(task_result.value());
}

std::string escapeScramUsername(const std::string& username)
{
    std::string escaped;
    escaped.reserve(username.size());

    for (char ch : username) {
        if (ch == '=') {
            escaped += "=3D";
        } else if (ch == ',') {
            escaped += "=2C";
        } else {
            escaped.push_back(ch);
        }
    }

    return escaped;
}

std::unordered_map<std::string, std::string> parseScramPayload(const std::string& payload)
{
    std::unordered_map<std::string, std::string> kv;

    size_t start = 0;
    while (start < payload.size()) {
        size_t comma = payload.find(',', start);
        if (comma == std::string::npos) {
            comma = payload.size();
        }

        const std::string item = payload.substr(start, comma - start);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) {
            kv[item.substr(0, eq)] = item.substr(eq + 1);
        }

        start = comma + 1;
    }

    return kv;
}

std::string base64Encode(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
        return "";
    }
    return galay::utils::Base64Util::Base64Encode(bytes.data(), bytes.size());
}

std::expected<std::vector<uint8_t>, MongoError> base64Decode(const std::string& text)
{
    if (text.empty()) {
        return std::vector<uint8_t>{};
    }

    std::string decoded;
    try {
        decoded = galay::utils::Base64Util::Base64Decode(text);
    } catch (const std::exception&) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, "base64 decode failed"));
    }

    return std::vector<uint8_t>(decoded.begin(), decoded.end());
}

std::expected<std::vector<uint8_t>, MongoError>
pbkdf2HmacSha256(const std::string& password,
                 const std::vector<uint8_t>& salt,
                 int iterations)
{
    if (iterations <= 0) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, "PKCS5_PBKDF2_HMAC failed"));
    }
    return galay::utils::PBKDF2::hmacSha256(password, salt, static_cast<uint32_t>(iterations), 32);
}

std::expected<std::vector<uint8_t>, MongoError>
hmacSha256(const std::vector<uint8_t>& key, const std::string& data)
{
    const auto digest = galay::utils::HMAC::hmacSha256(
        key.data(),
        key.size(),
        reinterpret_cast<const uint8_t*>(data.data()),
        data.size());
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::expected<std::vector<uint8_t>, MongoError> sha256(const std::vector<uint8_t>& data)
{
    const auto digest = galay::utils::SHA256::hash(data.data(), data.size());
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::vector<uint8_t> xorBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    const size_t size = std::min(a.size(), b.size());
    std::vector<uint8_t> out(size, 0);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return out;
}

std::expected<std::string, MongoError> generateClientNonce()
{
    std::vector<uint8_t> random_bytes = galay::utils::SaltGenerator::generateSecureBytes(18);
    return base64Encode(random_bytes);
}

std::expected<int32_t, MongoError> readConversationId(const MongoDocument& doc)
{
    const auto* field = doc.find("conversationId");
    if (field == nullptr) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Missing conversationId in SCRAM response"));
    }

    int64_t value = 0;
    if (field->isInt32()) {
        value = field->toInt32();
    } else if (field->isInt64()) {
        value = field->toInt64();
    } else if (field->isDouble()) {
        value = static_cast<int64_t>(field->toDouble());
    } else {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Invalid conversationId type in SCRAM response"));
    }

    if (value <= 0 || value > std::numeric_limits<int32_t>::max()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, "Invalid conversationId value"));
    }

    return static_cast<int32_t>(value);
}

std::expected<std::string, MongoError> readBinaryPayloadAsString(const MongoDocument& doc)
{
    const auto* payload_field = doc.find("payload");
    if (payload_field == nullptr || !payload_field->isBinary()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Missing payload in SCRAM response"));
    }

    const auto& payload = payload_field->toBinary();
    return std::string(payload.begin(), payload.end());
}

MongoDocument buildClientMetadata(const std::string& app_name)
{
    MongoDocument driver;
    driver.append("name", "galay-mongo");
    driver.append("version", "1.1.1");

    MongoDocument os;
#if defined(__APPLE__)
    os.append("type", "Darwin");
    os.append("name", "macOS");
#elif defined(__linux__)
    os.append("type", "Linux");
    os.append("name", "Linux");
#elif defined(_WIN32)
    os.append("type", "Windows");
    os.append("name", "Windows");
#else
    os.append("type", "Unknown");
    os.append("name", "Unknown");
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    os.append("architecture", "arm64");
#elif defined(__x86_64__) || defined(_M_X64)
    os.append("architecture", "x86_64");
#elif defined(__i386__) || defined(_M_IX86)
    os.append("architecture", "x86");
#endif

    MongoDocument client;
    if (!app_name.empty()) {
        MongoDocument app;
        app.append("name", app_name);
        client.append("application", app);
    }
    client.append("driver", driver);
    client.append("os", os);
    return client;
}

struct DecodeView
{
    const char* data = nullptr;
    int32_t msg_len = 0;
};

struct SendSegment
{
    const char* data = nullptr;
    size_t len = 0;
};

constexpr size_t kAsyncMaxMessageSize = 128 * 1024 * 1024;

std::expected<std::optional<DecodeView>, MongoError>
prepareDecodeView(std::span<const struct iovec> read_iovecs, std::string& parse_buffer)
{
    if (read_iovecs.empty()) {
        return std::optional<DecodeView>{};
    }

    const auto& first = read_iovecs.front();
    size_t total_len = first.iov_len;
    for (size_t i = 1; i < read_iovecs.size(); ++i) {
        total_len += read_iovecs[i].iov_len;
    }
    if (total_len < 4) {
        return std::optional<DecodeView>{};
    }

    int32_t msg_len = 0;
    if (first.iov_len >= 4) {
        msg_len = readInt32LE(static_cast<const char*>(first.iov_base));
    } else {
        char header_bytes[4];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            if (copied >= 4) {
                break;
            }
            const size_t chunk = std::min(iov.iov_len, static_cast<size_t>(4 - copied));
            std::memcpy(header_bytes + copied, iov.iov_base, chunk);
            copied += chunk;
        }
        msg_len = readInt32LE(header_bytes);
    }

    if (msg_len < 16) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Invalid Mongo message length in response"));
    }
    if (static_cast<size_t>(msg_len) > kAsyncMaxMessageSize) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Mongo message exceeds max supported size"));
    }
    if (total_len < static_cast<size_t>(msg_len)) {
        return std::optional<DecodeView>{};
    }

    if (first.iov_len >= static_cast<size_t>(msg_len)) {
        return std::optional<DecodeView>(
            DecodeView{static_cast<const char*>(first.iov_base), msg_len});
    }

    const size_t msg_size = static_cast<size_t>(msg_len);
    if (parse_buffer.size() < msg_size) {
        parse_buffer.resize(msg_size);
    }

    size_t copied = std::min(first.iov_len, msg_size);
    if (copied > 0) {
        std::memcpy(parse_buffer.data(), first.iov_base, copied);
    }

    for (size_t i = 1; i < read_iovecs.size() && copied < msg_size; ++i) {
        const auto& iov = read_iovecs[i];
        const size_t chunk = std::min(iov.iov_len, msg_size - copied);
        std::memcpy(parse_buffer.data() + copied, iov.iov_base, chunk);
        copied += chunk;
    }

    return std::optional<DecodeView>(DecodeView{parse_buffer.data(), msg_len});
}

void fillSendIovecsFromSegments(std::vector<struct iovec>& iovecs,
                                std::span<const SendSegment> segments,
                                size_t sent)
{
    iovecs.clear();
    size_t skip = sent;

    for (const auto& segment : segments) {
        if (segment.data == nullptr || segment.len == 0) {
            continue;
        }
        if (skip >= segment.len) {
            skip -= segment.len;
            continue;
        }

        iovecs.push_back(iovec{
            const_cast<char*>(segment.data + skip),
            segment.len - skip
        });
        skip = 0;
    }
}

bool isSimplePingCommand(const MongoDocument& command, const std::string& database)
{
    if (command.empty() || command.size() > 2) {
        return false;
    }

    const MongoValue* ping = command.find("ping");
    if (ping == nullptr) {
        return false;
    }

    const MongoValue* db = command.find("$db");
    if (db != nullptr) {
        if (command.size() != 2) {
            return false;
        }
        if (!db->isString() || db->toString() != database) {
            return false;
        }
    } else if (command.size() != 1) {
        return false;
    }

    if (ping->isInt32()) {
        return ping->toInt32() == 1;
    }
    if (ping->isInt64()) {
        return ping->toInt64() == 1;
    }
    if (ping->isDouble()) {
        return std::abs(ping->toDouble() - 1.0) < 1e-12;
    }
    return false;
}

MongoError makeServerError(MongoReply&& reply, std::string_view default_message)
{
    return MongoError(MONGO_ERROR_SERVER,
                      reply.errorCode(),
                      reply.errorMessage().empty()
                          ? std::string(default_message)
                          : reply.errorMessage());
}

} // namespace

struct AsyncMongoClientInternals
{
    enum class AuthPhase {
        HelloReply,
        SaslStartReply,
        SaslContinueReply,
        SaslFinalReply,
    };

    struct ConnectFlowState {
        AsyncMongoClient& client;
        MongoConfig config;
        bool auth_enabled = false;
        AuthPhase auth_phase = AuthPhase::HelloReply;
        std::string auth_db;
        int32_t auth_conversation_id = 0;
        std::string auth_client_nonce;
        std::string auth_client_first_bare;
        std::string auth_expected_server_signature;
        std::string encoded_request;

        ConnectFlowState(AsyncMongoClient& client_ref, MongoConfig cfg)
            : client(client_ref)
            , config(std::move(cfg))
        {
        }

        std::expected<void, MongoError> initialize()
        {
            auth_enabled = !config.username.empty() || !config.password.empty();
            auth_db = !config.auth_database.empty()
                ? config.auth_database
                : (!config.database.empty() ? config.database : "admin");

            if ((config.username.empty() && !config.password.empty()) ||
                (!config.username.empty() && config.password.empty())) {
                return std::unexpected(MongoError(
                    MONGO_ERROR_INVALID_PARAM,
                    "Both username and password are required for SCRAM authentication"));
            }

            MongoDocument hello;
            hello.append("hello", int32_t(1));
            hello.append("helloOk", true);
            hello.append("$db", config.hello_database.empty() ? std::string("admin")
                                                              : config.hello_database);
            hello.append("client", buildClientMetadata(config.app_name));

            encoded_request.clear();
            auto encoded = protocol::MongoProtocol::appendOpMsg(
                encoded_request,
                client.nextRequestId(),
                hello);
            if (!encoded) {
                return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
            }
            return {};
        }

        std::expected<bool, MongoError> handleReply(MongoReply&& reply)
        {
            switch (auth_phase) {
            case AuthPhase::HelloReply:
                return handleHelloReply(std::move(reply));
            case AuthPhase::SaslStartReply:
                return handleSaslStartReply(std::move(reply));
            case AuthPhase::SaslContinueReply:
                return handleSaslContinueReply(std::move(reply));
            case AuthPhase::SaslFinalReply:
                return handleSaslFinalReply(std::move(reply));
            }

            return std::unexpected(MongoError(MONGO_ERROR_INTERNAL,
                                              "Unknown auth phase in connect flow"));
        }

    private:
        std::expected<bool, MongoError> handleHelloReply(MongoReply&&)
        {
            if (!auth_enabled) {
                return true;
            }

            auto nonce_or_err = generateClientNonce();
            if (!nonce_or_err) {
                return std::unexpected(nonce_or_err.error());
            }

            auth_client_nonce = std::move(nonce_or_err.value());
            auth_client_first_bare =
                "n=" + escapeScramUsername(config.username) + ",r=" + auth_client_nonce;
            const std::string client_first_message = "n,," + auth_client_first_bare;

            MongoValue::Binary payload(client_first_message.begin(), client_first_message.end());

            MongoDocument sasl_start;
            sasl_start.append("saslStart", int32_t(1));
            sasl_start.append("mechanism", "SCRAM-SHA-256");
            sasl_start.append("payload", std::move(payload));
            sasl_start.append("autoAuthorize", int32_t(1));
            sasl_start.append("$db", auth_db);

            encoded_request.clear();
            auto encoded = protocol::MongoProtocol::appendOpMsg(
                encoded_request,
                client.nextRequestId(),
                sasl_start);
            if (!encoded) {
                return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
            }
            auth_phase = AuthPhase::SaslStartReply;
            return false;
        }

        std::expected<bool, MongoError> handleSaslStartReply(MongoReply&& reply)
        {
            const auto& doc = reply.document();

            auto conversation_id_or_err = readConversationId(doc);
            if (!conversation_id_or_err) {
                return std::unexpected(conversation_id_or_err.error());
            }
            auth_conversation_id = conversation_id_or_err.value();

            auto server_first_or_err = readBinaryPayloadAsString(doc);
            if (!server_first_or_err) {
                return std::unexpected(server_first_or_err.error());
            }
            const std::string server_first_message = std::move(server_first_or_err.value());

            const auto kv = parseScramPayload(server_first_message);
            const auto nonce_it = kv.find("r");
            const auto salt_it = kv.find("s");
            const auto iter_it = kv.find("i");
            if (nonce_it == kv.end() || salt_it == kv.end() || iter_it == kv.end()) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "Invalid SCRAM server-first-message"));
            }

            const std::string& server_nonce = nonce_it->second;
            if (server_nonce.rfind(auth_client_nonce, 0) != 0) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "SCRAM server nonce does not include client nonce"));
            }

            int iterations = 0;
            const auto parse_iter_result = std::from_chars(
                iter_it->second.data(),
                iter_it->second.data() + iter_it->second.size(),
                iterations);
            if (parse_iter_result.ec != std::errc{} || iterations <= 0) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "Invalid SCRAM iteration count"));
            }

            auto salt_or_err = base64Decode(salt_it->second);
            if (!salt_or_err) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "Invalid SCRAM salt: " +
                                                      salt_or_err.error().message()));
            }

            const std::string client_final_without_proof = "c=biws,r=" + server_nonce;
            const std::string auth_message = auth_client_first_bare + "," +
                                             server_first_message + "," +
                                             client_final_without_proof;

            auto salted_password =
                pbkdf2HmacSha256(config.password, salt_or_err.value(), iterations);
            if (!salted_password) {
                return std::unexpected(salted_password.error());
            }

            auto client_key = hmacSha256(salted_password.value(), "Client Key");
            if (!client_key) {
                return std::unexpected(client_key.error());
            }

            auto stored_key = sha256(client_key.value());
            if (!stored_key) {
                return std::unexpected(stored_key.error());
            }

            auto client_signature = hmacSha256(stored_key.value(), auth_message);
            if (!client_signature) {
                return std::unexpected(client_signature.error());
            }

            auto server_key = hmacSha256(salted_password.value(), "Server Key");
            if (!server_key) {
                return std::unexpected(server_key.error());
            }

            auto server_signature = hmacSha256(server_key.value(), auth_message);
            if (!server_signature) {
                return std::unexpected(server_signature.error());
            }

            const auto client_proof = xorBytes(client_key.value(), client_signature.value());
            auth_expected_server_signature = base64Encode(server_signature.value());
            if (auth_expected_server_signature.empty()) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "Failed to encode SCRAM server signature"));
            }

            const std::string client_final_message =
                client_final_without_proof + ",p=" + base64Encode(client_proof);

            MongoValue::Binary continue_payload(client_final_message.begin(),
                                                client_final_message.end());
            MongoDocument sasl_continue;
            sasl_continue.append("saslContinue", int32_t(1));
            sasl_continue.append("conversationId", auth_conversation_id);
            sasl_continue.append("payload", std::move(continue_payload));
            sasl_continue.append("$db", auth_db);

            encoded_request.clear();
            auto encoded = protocol::MongoProtocol::appendOpMsg(
                encoded_request,
                client.nextRequestId(),
                sasl_continue);
            if (!encoded) {
                return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
            }
            auth_phase = AuthPhase::SaslContinueReply;
            return false;
        }

        std::expected<bool, MongoError> handleSaslContinueReply(MongoReply&& reply)
        {
            const auto& doc = reply.document();

            auto server_final_or_err = readBinaryPayloadAsString(doc);
            if (!server_final_or_err) {
                return std::unexpected(server_final_or_err.error());
            }

            const auto kv = parseScramPayload(server_final_or_err.value());
            const auto error_it = kv.find("e");
            if (error_it != kv.end()) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "SCRAM server-final-message error: " +
                                                      error_it->second));
            }

            const auto verifier_it = kv.find("v");
            if (verifier_it == kv.end()) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "SCRAM server-final-message missing verifier"));
            }

            if (verifier_it->second != auth_expected_server_signature) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "SCRAM server signature mismatch"));
            }

            if (doc.getBool("done", false)) {
                return true;
            }

            MongoDocument final_continue;
            final_continue.append("saslContinue", int32_t(1));
            final_continue.append("conversationId", auth_conversation_id);
            final_continue.append("payload", MongoValue::Binary{});
            final_continue.append("$db", auth_db);

            encoded_request.clear();
            auto encoded = protocol::MongoProtocol::appendOpMsg(
                encoded_request,
                client.nextRequestId(),
                final_continue);
            if (!encoded) {
                return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
            }
            auth_phase = AuthPhase::SaslFinalReply;
            return false;
        }

        std::expected<bool, MongoError> handleSaslFinalReply(MongoReply&& reply)
        {
            if (!reply.document().getBool("done", false)) {
                return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                                  "SCRAM authentication not finished"));
            }
            return true;
        }
    };

    static Task<std::expected<size_t, IOError>> writevOnce(AsyncMongoClient& client,
                                                           std::array<struct iovec, 3>& iovecs,
                                                           size_t count)
    {
        auto awaitable = client.m_socket.writev(iovecs, count);
        if (client.m_config.isSendTimeoutEnabled()) {
            co_return co_await awaitable.timeout(client.m_config.send_timeout);
        }
        co_return co_await awaitable;
    }

    static Task<std::expected<size_t, IOError>> readvOnce(AsyncMongoClient& client,
                                                          std::array<struct iovec, 2>& iovecs,
                                                          size_t count)
    {
        auto awaitable = client.m_socket.readv(iovecs, count);
        if (client.m_config.isRecvTimeoutEnabled()) {
            co_return co_await awaitable.timeout(client.m_config.recv_timeout);
        }
        co_return co_await awaitable;
    }

    static Task<std::expected<void, MongoError>> connectSocket(AsyncMongoClient& client,
                                                               const MongoConfig& config)
    {
        client.m_socket.option().handleNonBlock();

        galay::kernel::Host host(galay::kernel::IPType::IPV4, config.host, config.port);
        auto connect_result = co_await client.m_socket.connect(host);
        if (!connect_result.has_value()) {
            co_return std::unexpected(mapIoError(connect_result.error(), MONGO_ERROR_CONNECTION));
        }

        if (config.tcp_nodelay) {
            auto nodelay_result = client.m_socket.option().handleTcpNoDelay();
            if (!nodelay_result.has_value()) {
                co_return std::unexpected(mapIoError(nodelay_result.error(), MONGO_ERROR_CONNECTION));
            }
        }
        co_return std::expected<void, MongoError>{};
    }

    static std::expected<std::optional<protocol::MongoMessage>, MongoError>
    tryParseMessage(AsyncMongoClient& client)
    {
        struct iovec read_iovecs[2];
        const size_t read_iovecs_count = client.m_ring_buffer.getReadIovecs(read_iovecs, 2);
        if (read_iovecs_count == 0) {
            return std::optional<protocol::MongoMessage>{};
        }

        auto decode_view_or_err = prepareDecodeView(
            std::span<const struct iovec>(read_iovecs, read_iovecs_count),
            client.m_decode_scratch);
        if (!decode_view_or_err) {
            return std::unexpected(decode_view_or_err.error());
        }
        if (!decode_view_or_err->has_value()) {
            return std::optional<protocol::MongoMessage>{};
        }

        const DecodeView& view = decode_view_or_err->value();
        auto message =
            protocol::MongoProtocol::decodeMessage(view.data, static_cast<size_t>(view.msg_len));
        if (!message) {
            return std::unexpected(message.error());
        }

        client.m_ring_buffer.consume(static_cast<size_t>(view.msg_len));
        return std::optional<protocol::MongoMessage>(std::move(message.value()));
    }

    static Task<std::expected<protocol::MongoMessage, MongoError>>
    recvMessage(AsyncMongoClient& client,
                MongoErrorType io_error_type,
                std::string_view closed_message,
                std::string_view no_space_message)
    {
        while (true) {
            auto message_or_err = tryParseMessage(client);
            if (!message_or_err) {
                co_return std::unexpected(std::move(message_or_err.error()));
            }
            if (message_or_err->has_value()) {
                co_return std::move(message_or_err->value());
            }

            struct iovec read_iovecs[2];
            const size_t count = client.m_ring_buffer.getWriteIovecs(read_iovecs, 2);
            if (count == 0) {
                co_return std::unexpected(
                    MongoError(MONGO_ERROR_RECV, std::string(no_space_message)));
            }

            std::array<struct iovec, 2> borrowed_iovecs{};
            for (size_t i = 0; i < count; ++i) {
                borrowed_iovecs[i] = read_iovecs[i];
            }

            auto read_result = flattenIoTaskResult(
                co_await readvOnce(client, borrowed_iovecs, count),
                io_error_type);
            if (!read_result.has_value()) {
                co_return std::unexpected(std::move(read_result.error()));
            }
            if (read_result.value() == 0) {
                co_return std::unexpected(
                    MongoError(MONGO_ERROR_CONNECTION_CLOSED, std::string(closed_message)));
            }
            client.m_ring_buffer.produce(read_result.value());
        }
    }

    static Task<std::expected<bool, MongoError>> sendSegments(
        AsyncMongoClient& client,
        std::span<const SendSegment> segments,
        MongoErrorType io_error_type,
        std::string_view closed_message)
    {
        size_t total_len = 0;
        for (const auto& segment : segments) {
            total_len += segment.len;
        }

        size_t sent = 0;
        std::vector<struct iovec> iovecs;
        iovecs.reserve(segments.size());

        while (sent < total_len) {
            fillSendIovecsFromSegments(iovecs, segments, sent);
            if (iovecs.empty()) {
                co_return std::unexpected(MongoError(MONGO_ERROR_INTERNAL,
                                                     "sendSegments produced empty iovecs"));
            }

            std::array<struct iovec, 3> borrowed_iovecs{};
            for (size_t i = 0; i < iovecs.size(); ++i) {
                borrowed_iovecs[i] = iovecs[i];
            }

            auto write_result = flattenIoTaskResult(
                co_await writevOnce(client, borrowed_iovecs, iovecs.size()),
                io_error_type);
            if (!write_result.has_value()) {
                co_return std::unexpected(std::move(write_result.error()));
            }
            if (write_result.value() == 0) {
                co_return std::unexpected(
                    MongoError(MONGO_ERROR_CONNECTION_CLOSED, std::string(closed_message)));
            }

            sent += write_result.value();
        }

        co_return true;
    }

    static MongoConnectAwaitable connect(AsyncMongoClient& client, MongoConfig config)
    {
        client.m_ring_buffer.clear();
        client.m_decode_scratch.clear();

        ConnectFlowState state(client, std::move(config));
        auto init_result = state.initialize();
        if (!init_result.has_value()) {
            co_return std::unexpected(std::move(init_result.error()));
        }

        auto connect_result = flattenMongoTaskResult(
            co_await connectSocket(client, state.config),
            MONGO_ERROR_CONNECTION);
        if (!connect_result.has_value()) {
            co_return std::unexpected(std::move(connect_result.error()));
        }

        while (true) {
            const std::array<SendSegment, 1> segments{{
                SendSegment{state.encoded_request.data(), state.encoded_request.size()}
            }};
            auto send_result = flattenMongoTaskResult(
                co_await sendSegments(
                    client,
                    std::span<const SendSegment>(segments),
                    MONGO_ERROR_SEND,
                    "Connection closed during connect/auth request send"),
                MONGO_ERROR_SEND);
            if (!send_result.has_value()) {
                co_return std::unexpected(std::move(send_result.error()));
            }

            auto message_result = flattenMongoTaskResult(
                co_await recvMessage(
                    client,
                    MONGO_ERROR_RECV,
                    "Connection closed while receiving connect/auth reply",
                    "No writable ring buffer space while receiving connect/auth reply"),
                MONGO_ERROR_RECV);
            if (!message_result.has_value()) {
                co_return std::unexpected(std::move(message_result.error()));
            }

            MongoReply reply(std::move(message_result->body));
            if (!reply.ok()) {
                co_return std::unexpected(makeServerError(
                    std::move(reply),
                    "Mongo connect/auth command failed"));
            }

            auto next_result = state.handleReply(std::move(reply));
            if (!next_result.has_value()) {
                co_return std::unexpected(std::move(next_result.error()));
            }
            if (next_result.value()) {
                client.m_is_closed = false;
                if (state.auth_enabled) {
                    MONGO_LOG_INFO("[client]", "Mongo connected and authenticated successfully to {}:{}",
                                 state.config.host,
                                 state.config.port);
                } else {
                    MONGO_LOG_INFO("[client]", "Mongo connected successfully to {}:{}",
                                 state.config.host,
                                 state.config.port);
                }
                co_return true;
            }
        }
    }

    static MongoCommandAwaitable command(AsyncMongoClient& client,
                                         std::string database,
                                         MongoDocument command)
    {
        if (client.m_is_closed) {
            co_return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                                 "Mongo client is not connected"));
        }

        const int32_t request_id = client.nextRequestId();
        std::array<char, 4> request_id_le{};
        writeInt32LE(request_id_le.data(), request_id);

        std::string encoded_request;
        std::array<SendSegment, 3> send_segments{};
        size_t send_segment_count = 0;

        if (isSimplePingCommand(command, database)) {
            if (client.m_ping_encoded_template.empty() || client.m_ping_template_db != database) {
                client.m_ping_template_db = database;
                client.m_ping_encoded_template.clear();
                auto encoded = protocol::MongoProtocol::appendOpMsgWithDatabase(
                    client.m_ping_encoded_template,
                    0,
                    command,
                    database);
                if (!encoded) {
                    co_return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
                }
            }

            if (client.m_ping_encoded_template.size() < 8) {
                co_return std::unexpected(MongoError(MONGO_ERROR_INTERNAL,
                                                     "Invalid cached ping template"));
            }

            send_segments[0] = SendSegment{client.m_ping_encoded_template.data(), 4};
            send_segments[1] = SendSegment{request_id_le.data(), request_id_le.size()};
            send_segments[2] = SendSegment{
                client.m_ping_encoded_template.data() + 8,
                client.m_ping_encoded_template.size() - 8
            };
            send_segment_count = 3;
        } else {
            auto encoded = protocol::MongoProtocol::appendOpMsgWithDatabase(
                encoded_request,
                request_id,
                command,
                database);
            if (!encoded) {
                co_return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
            }
            send_segments[0] = SendSegment{encoded_request.data(), encoded_request.size()};
            send_segment_count = 1;
        }

        auto send_result = flattenMongoTaskResult(
            co_await sendSegments(
                client,
                std::span<const SendSegment>(send_segments.data(), send_segment_count),
                MONGO_ERROR_SEND,
                "Connection closed during command send"),
            MONGO_ERROR_SEND);
        if (!send_result.has_value()) {
            co_return std::unexpected(std::move(send_result.error()));
        }

        auto message_result = flattenMongoTaskResult(
            co_await recvMessage(
                client,
                MONGO_ERROR_RECV,
                "Connection closed while receiving command reply",
                "No writable ring buffer space while receiving command reply"),
            MONGO_ERROR_RECV);
        if (!message_result.has_value()) {
            co_return std::unexpected(std::move(message_result.error()));
        }
        if (message_result->header.response_to != request_id) {
            co_return std::unexpected(MongoError(
                MONGO_ERROR_PROTOCOL,
                "Response responseTo does not match sent requestId"));
        }

        MongoReply reply(std::move(message_result->body));
        if (!reply.ok()) {
            co_return std::unexpected(makeServerError(std::move(reply), "Mongo command failed"));
        }

        co_return std::move(reply);
    }

    static MongoPipelineAwaitable pipeline(AsyncMongoClient& client,
                                           std::string database,
                                           std::span<const MongoDocument> commands)
    {
        if (client.m_is_closed) {
            co_return std::unexpected(MongoError(MONGO_ERROR_CONNECTION,
                                                 "Mongo client is not connected"));
        }
        if (commands.empty()) {
            co_return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM,
                                                 "Pipeline commands must not be empty"));
        }
        if (commands.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            co_return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM,
                                                 "Pipeline commands exceed supported size"));
        }

        std::vector<MongoPipelineResponse> responses(commands.size());
        const int32_t first_request_id = client.reserveRequestIdBlock(commands.size());
        for (size_t i = 0; i < commands.size(); ++i) {
            responses[i].request_id = static_cast<int32_t>(
                static_cast<int64_t>(first_request_id) + static_cast<int64_t>(i));
        }

        auto encoded_batch = protocol::MongoCommandBuilder::encodePipeline(
            database,
            first_request_id,
            commands,
            client.m_pipeline_reserve_per_command);
        if (!encoded_batch) {
            co_return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded_batch.error()));
        }

        const std::array<SendSegment, 1> send_segments{{
            SendSegment{encoded_batch->data(), encoded_batch->size()}
        }};
        auto send_result = flattenMongoTaskResult(
            co_await sendSegments(
                client,
                std::span<const SendSegment>(send_segments),
                MONGO_ERROR_SEND,
                "Connection closed during pipeline send"),
            MONGO_ERROR_SEND);
        if (!send_result.has_value()) {
            co_return std::unexpected(std::move(send_result.error()));
        }

        size_t received = 0;
        while (received < responses.size()) {
            auto message_result = flattenMongoTaskResult(
                co_await recvMessage(
                    client,
                    MONGO_ERROR_RECV,
                    "Connection closed while receiving pipeline replies",
                    "No writable ring buffer space while receiving pipeline replies"),
                MONGO_ERROR_RECV);
            if (!message_result.has_value()) {
                co_return std::unexpected(std::move(message_result.error()));
            }

            const int32_t response_to = message_result->header.response_to;
            if (response_to <= 0) {
                co_return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                                     "Pipeline response has invalid responseTo"));
            }

            const int64_t index_i64 =
                static_cast<int64_t>(response_to) - static_cast<int64_t>(first_request_id);
            if (index_i64 < 0 || index_i64 >= static_cast<int64_t>(responses.size())) {
                co_return std::unexpected(MongoError(
                    MONGO_ERROR_PROTOCOL,
                    "Pipeline responseTo does not match any in-flight requestId"));
            }

            auto& slot = responses[static_cast<size_t>(index_i64)];
            if (slot.request_id != response_to) {
                co_return std::unexpected(MongoError(
                    MONGO_ERROR_PROTOCOL,
                    "Pipeline responseTo does not map to expected requestId"));
            }
            if (slot.reply.has_value() || slot.error.has_value()) {
                co_return std::unexpected(MongoError(
                    MONGO_ERROR_PROTOCOL,
                    "Pipeline received duplicate response for the same requestId"));
            }

            MongoReply reply(std::move(message_result->body));
            if (reply.ok()) {
                slot.reply = std::move(reply);
            } else {
                slot.error = makeServerError(std::move(reply), "Mongo pipeline command failed");
            }

            ++received;
        }

        co_return responses;
    }
};

int32_t AsyncMongoClient::reserveRequestIdBlock(size_t count)
{
    if (count == 0) {
        count = 1;
    }

    if (m_next_request_id <= 0) {
        m_next_request_id = 1;
    }

    const int64_t max_request_id = std::numeric_limits<int32_t>::max();
    const int64_t first_candidate = static_cast<int64_t>(m_next_request_id);
    if (first_candidate + static_cast<int64_t>(count) - 1 > max_request_id) {
        m_next_request_id = 1;
    }

    const int32_t first = m_next_request_id;
    const int64_t next_candidate = static_cast<int64_t>(first) + static_cast<int64_t>(count);
    if (next_candidate > max_request_id) {
        m_next_request_id = 1;
    } else {
        m_next_request_id = static_cast<int32_t>(next_candidate);
    }

    return first;
}

int32_t AsyncMongoClient::nextRequestId()
{
    return reserveRequestIdBlock(1);
}

AsyncMongoClient::AsyncMongoClient(IOScheduler* scheduler,
                                   AsyncMongoConfig config)
    : m_config(std::move(config))
    , m_ring_buffer(m_config.buffer_size > 0 ? m_config.buffer_size
                                             : galay::utils::RingBuffer::kDefaultCapacity)
    , m_pipeline_reserve_per_command(std::max<size_t>(32, m_config.pipeline_reserve_per_command))
{
    (void)scheduler;
}

AsyncMongoClient::AsyncMongoClient(AsyncMongoClient&& other) noexcept
    : m_is_closed(other.m_is_closed)
    , m_config(std::move(other.m_config))
    , m_socket(std::move(other.m_socket))
    , m_ring_buffer(std::move(other.m_ring_buffer))
    , m_decode_scratch(std::move(other.m_decode_scratch))
    , m_ping_template_db(std::move(other.m_ping_template_db))
    , m_ping_encoded_template(std::move(other.m_ping_encoded_template))
    , m_pipeline_reserve_per_command(other.m_pipeline_reserve_per_command)
    , m_next_request_id(other.m_next_request_id)
{
    other.m_is_closed = true;
}

AsyncMongoClient& AsyncMongoClient::operator=(AsyncMongoClient&& other) noexcept
{
    if (this != &other) {
        if (!m_is_closed) {
            m_is_closed = true;
            m_socket.close();
        }
        m_is_closed = other.m_is_closed;
        m_config = std::move(other.m_config);
        m_socket = std::move(other.m_socket);
        m_ring_buffer = std::move(other.m_ring_buffer);
        m_decode_scratch = std::move(other.m_decode_scratch);
        m_ping_template_db = std::move(other.m_ping_template_db);
        m_ping_encoded_template = std::move(other.m_ping_encoded_template);
        m_pipeline_reserve_per_command = other.m_pipeline_reserve_per_command;
        m_next_request_id = other.m_next_request_id;
        other.m_is_closed = true;
    }
    return *this;
}

MongoConnectAwaitable AsyncMongoClient::connect(MongoConfig config)
{
    return AsyncMongoClientInternals::connect(*this, std::move(config));
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
    return AsyncMongoClientInternals::command(*this, std::move(database), std::move(command));
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
    return AsyncMongoClientInternals::pipeline(*this, std::move(database), commands);
}

} // namespace galay::mongo
