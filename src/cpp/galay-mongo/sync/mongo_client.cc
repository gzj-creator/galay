#include "mongo_client.h"

#include "../../galay-utils/crypto/hmac.hpp"
#include "../../galay-utils/crypto/pbkdf2.hpp"
#include "../../galay-utils/crypto/salt.hpp"
#include "../../galay-utils/encoding/base64.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <thread>

namespace galay::mongo
{

namespace
{

MongoDocument build_client_metadata(const std::string& app_name)
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
        client.append("application", std::move(app));
    }
    client.append("driver", std::move(driver));
    client.append("os", std::move(os));
    return client;
}

struct MongoServerCandidate
{
    MongoEndpoint endpoint;
    std::chrono::steady_clock::duration round_trip_time{};
    bool primary = false;
    bool secondary = false;
};

std::optional<MongoEndpoint> parse_advertised_endpoint(const std::string& text)
{
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return std::nullopt;
    }

    uint32_t port = 0;
    const char* begin = text.data() + colon + 1;
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0 || port > 65535) {
        return std::nullopt;
    }

    MongoEndpoint endpoint;
    endpoint.host = text.substr(0, colon);
    endpoint.port = static_cast<uint16_t>(port);
    return endpoint;
}

void append_endpoint_if_missing(std::vector<MongoEndpoint>& endpoints, MongoEndpoint endpoint)
{
    const auto duplicate = std::find_if(
        endpoints.begin(),
        endpoints.end(),
        [&endpoint](const MongoEndpoint& current) {
            return current.host == endpoint.host && current.port == endpoint.port;
        });
    if (duplicate == endpoints.end()) {
        endpoints.push_back(std::move(endpoint));
    }
}

void append_advertised_endpoints(const MongoDocument& hello,
                               std::vector<MongoEndpoint>& endpoints)
{
    const auto append_text = [&endpoints](const std::string& text) {
        auto endpoint = parse_advertised_endpoint(text);
        if (endpoint) {
            append_endpoint_if_missing(endpoints, std::move(*endpoint));
        }
    };

    if (const MongoValue* hosts = hello.find("hosts"); hosts != nullptr && hosts->is_array()) {
        for (const auto& host : hosts->to_array().values()) {
            if (host.is_string()) {
                append_text(host.to_string());
            }
        }
    }
    if (const MongoValue* passives = hello.find("passives");
        passives != nullptr && passives->is_array()) {
        for (const auto& host : passives->to_array().values()) {
            if (host.is_string()) {
                append_text(host.to_string());
            }
        }
    }

    const std::string primary = hello.get_string("primary");
    if (!primary.empty()) {
        append_text(primary);
    }
}

bool server_matches_preference(const MongoServerCandidate& candidate,
                             MongoReadPreference preference)
{
    switch (preference) {
    case MongoReadPreference::kPrimary:
        return candidate.primary;
    case MongoReadPreference::kPrimaryPreferred:
    case MongoReadPreference::kSecondaryPreferred:
    case MongoReadPreference::kNearest:
        return candidate.primary || candidate.secondary;
    case MongoReadPreference::kSecondary:
        return candidate.secondary;
    }
    return false;
}

const MongoServerCandidate* select_server(
    const std::vector<MongoServerCandidate>& candidates,
    MongoReadPreference preference)
{
    const auto fastest = [&candidates](bool primary, bool secondary) {
        const MongoServerCandidate* selected = nullptr;
        for (const auto& candidate : candidates) {
            if ((primary && candidate.primary) || (secondary && candidate.secondary)) {
                if (selected == nullptr ||
                    candidate.round_trip_time < selected->round_trip_time) {
                    selected = &candidate;
                }
            }
        }
        return selected;
    };

    switch (preference) {
    case MongoReadPreference::kPrimary:
        return fastest(true, false);
    case MongoReadPreference::kPrimaryPreferred:
        if (const auto* primary = fastest(true, false); primary != nullptr) {
            return primary;
        }
        return fastest(false, true);
    case MongoReadPreference::kSecondary:
        return fastest(false, true);
    case MongoReadPreference::kSecondaryPreferred:
        if (const auto* secondary = fastest(false, true); secondary != nullptr) {
            return secondary;
        }
        return fastest(true, false);
    case MongoReadPreference::kNearest:
        return fastest(true, true);
    }
    return nullptr;
}

} // namespace

MongoClient::MongoClient() = default;

MongoClient::~MongoClient()
{
    close();
}

MongoClient::MongoClient(MongoClient&& other) noexcept
    : m_connection(std::move(other.m_connection))
    , m_config(std::move(other.m_config))
    , m_next_request_id(other.m_next_request_id)
{
    other.m_next_request_id = 1;
}

MongoClient& MongoClient::operator=(MongoClient&& other) noexcept
{
    if (this != &other) {
        close();
        m_connection = std::move(other.m_connection);
        m_config = std::move(other.m_config);
        m_next_request_id = other.m_next_request_id;
        other.m_next_request_id = 1;
    }
    return *this;
}

MongoVoidResult MongoClient::connect(const MongoConfig& config)
{
    const auto connect_and_hello = [this](const MongoConfig& candidate) -> MongoResult {
        m_config = candidate;
        const auto conn_options = protocol::Connection::ConnectOptions::from_mongo_config(candidate);
        auto connected = m_connection.connect(conn_options);
        if (!connected) {
            return std::unexpected(connected.error());
        }

        m_next_request_id = 1;
        MongoDocument hello;
        hello.append("hello", int32_t(1));
        hello.append("helloOk", true);
        hello.append("client", build_client_metadata(candidate.app_name));
        const std::string hello_db = candidate.hello_database.empty()
            ? "admin"
            : candidate.hello_database;
        return run_command_request(hello_db, hello, true);
    };

    const bool topology_requested =
        !config.seeds.empty() ||
        !config.topology.replica_set_name.empty() ||
        config.topology.read_preference != MongoReadPreference::kPrimary;
    if (!topology_requested) {
        auto hello_result = connect_and_hello(config);
        if (!hello_result) {
            close();
            return std::unexpected(hello_result.error());
        }

        auto auth_result = authenticate_if_needed(config);
        if (!auth_result) {
            close();
            return auth_result;
        }
        return {};
    }

    std::vector<MongoEndpoint> endpoints = config.seeds;
    if (endpoints.empty()) {
        endpoints.push_back({config.host, config.port});
    }

    const auto timeout = std::max(config.topology.server_selection_timeout,
                                  std::chrono::milliseconds(0));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::optional<MongoError> last_error;
    bool first_scan = true;

    while (first_scan || std::chrono::steady_clock::now() < deadline) {
        first_scan = false;
        std::vector<MongoServerCandidate> candidates;

        for (size_t index = 0; index < endpoints.size(); ++index) {
            MongoConfig candidate_config = config;
            candidate_config.host = endpoints[index].host;
            candidate_config.port = endpoints[index].port;

            const auto now = std::chrono::steady_clock::now();
            if (now < deadline) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
                const auto remaining_count = std::max<int64_t>(1, remaining.count());
                candidate_config.connect_timeout_ms = static_cast<uint32_t>(
                    std::min<int64_t>(candidate_config.connect_timeout_ms, remaining_count));
            } else if (timeout == std::chrono::milliseconds(0)) {
                candidate_config.connect_timeout_ms = 1;
            }

            const auto started = std::chrono::steady_clock::now();
            auto hello_result = connect_and_hello(candidate_config);
            const auto round_trip_time = std::chrono::steady_clock::now() - started;
            if (!hello_result) {
                last_error = hello_result.error();
                close();
                continue;
            }

            const MongoDocument& hello = hello_result->document();
            append_advertised_endpoints(hello, endpoints);

            const std::string set_name = hello.get_string("setName");
            if (!config.topology.replica_set_name.empty() &&
                set_name != config.topology.replica_set_name) {
                last_error = MongoError(
                    MONGO_ERROR_SERVER,
                    "Replica set mismatch for " + candidate_config.host + ":" +
                        std::to_string(candidate_config.port));
                close();
                continue;
            }

            MongoServerCandidate candidate;
            candidate.endpoint = endpoints[index];
            candidate.round_trip_time = round_trip_time;
            candidate.primary = hello.get_bool("isWritablePrimary", false) ||
                                hello.get_bool("ismaster", false);
            candidate.secondary = hello.get_bool("secondary", false);
            if (candidate.primary || candidate.secondary) {
                candidates.push_back(std::move(candidate));
            }
            close();
        }

        const MongoServerCandidate* selected =
            select_server(candidates, config.topology.read_preference);
        if (selected != nullptr) {
            MongoConfig selected_config = config;
            selected_config.host = selected->endpoint.host;
            selected_config.port = selected->endpoint.port;

            auto hello_result = connect_and_hello(selected_config);
            if (hello_result) {
                const MongoDocument& hello = hello_result->document();
                const std::string set_name = hello.get_string("setName");
                MongoServerCandidate confirmed;
                confirmed.endpoint = selected->endpoint;
                confirmed.primary = hello.get_bool("isWritablePrimary", false) ||
                                    hello.get_bool("ismaster", false);
                confirmed.secondary = hello.get_bool("secondary", false);

                const bool set_matches = config.topology.replica_set_name.empty() ||
                    set_name == config.topology.replica_set_name;
                if (set_matches &&
                    server_matches_preference(confirmed, config.topology.read_preference)) {
                    auto auth_result = authenticate_if_needed(selected_config);
                    if (!auth_result) {
                        close();
                        return auth_result;
                    }
                    return {};
                }

                last_error = MongoError(MONGO_ERROR_SERVER,
                                        "Selected MongoDB server changed role or replica set");
            } else {
                last_error = hello_result.error();
            }
            close();
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::string message = "MongoDB server selection timed out";
    if (last_error) {
        message += ": " + last_error->message();
    }
    return std::unexpected(MongoError(MONGO_ERROR_TIMEOUT, std::move(message)));
}

MongoVoidResult MongoClient::connect(const std::string& host,
                                      uint16_t port,
                                      const std::string& database)
{
    MongoConfig config = MongoConfig::create(host, port, database);
    return connect(config);
}

MongoResult MongoClient::command(const std::string& database, const MongoDocument& command)
{
    return run_command_request(database, command, true);
}

MongoResult MongoClient::ping(const std::string& database)
{
    MongoDocument command;
    command.append("ping", int32_t(1));
    return run_command_request(database, command, true);
}

MongoResult MongoClient::find_one(const std::string& database,
                                  const std::string& collection,
                                  const MongoDocument& filter,
                                  const MongoDocument& projection)
{
    MongoDocument command;
    command.append("find", collection);
    command.append("filter", filter.clone());
    command.append("limit", int32_t(1));
    if (!projection.empty()) {
        command.append("projection", projection.clone());
    }
    return run_command_request(database, command, true);
}

MongoResult MongoClient::insert_one(const std::string& database,
                                    const std::string& collection,
                                    const MongoDocument& document)
{
    MongoArray documents;
    documents.append(document.clone());

    MongoDocument command;
    command.append("insert", collection);
    command.append("documents", std::move(documents));
    command.append("ordered", true);
    return run_command_request(database, command, true);
}

MongoResult MongoClient::update_one(const std::string& database,
                                    const std::string& collection,
                                    const MongoDocument& filter,
                                    const MongoDocument& update,
                                    bool upsert)
{
    MongoDocument update_item;
    update_item.append("q", filter.clone());
    update_item.append("u", update.clone());
    update_item.append("multi", false);
    update_item.append("upsert", upsert);

    MongoArray updates;
    updates.append(std::move(update_item));

    MongoDocument command;
    command.append("update", collection);
    command.append("updates", std::move(updates));
    command.append("ordered", true);
    return run_command_request(database, command, true);
}

MongoResult MongoClient::delete_one(const std::string& database,
                                    const std::string& collection,
                                    const MongoDocument& filter)
{
    MongoDocument delete_item;
    delete_item.append("q", filter.clone());
    delete_item.append("limit", int32_t(1));

    MongoArray deletes;
    deletes.append(std::move(delete_item));

    MongoDocument command;
    command.append("delete", collection);
    command.append("deletes", std::move(deletes));
    command.append("ordered", true);
    return run_command_request(database, command, true);
}

void MongoClient::close()
{
    m_connection.disconnect();
}

MongoResult MongoClient::run_command_request(const std::string& database,
                                           const MongoDocument& command,
                                           bool check_ok)
{
    if (!m_connection.is_connected()) {
        return std::unexpected(MongoError(MONGO_ERROR_CONNECTION_CLOSED, "Not connected"));
    }

    MongoDocument request = command.clone();
    if (!request.has("$db")) {
        request.append("$db", database);
    }

    if (m_next_request_id <= 0) {
        m_next_request_id = 1;
    }
    const int32_t request_id = m_next_request_id;
    if (m_next_request_id == std::numeric_limits<int32_t>::max()) {
        m_next_request_id = 1;
    } else {
        ++m_next_request_id;
    }
    m_encoded_request_buffer.clear();
    auto encoded = protocol::MongoProtocol::append_op_msg(m_encoded_request_buffer, request_id, request);
    if (!encoded) {
        return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, encoded.error()));
    }

    auto sent = m_connection.send(m_encoded_request_buffer);
    if (!sent) {
        return std::unexpected(sent.error());
    }

    auto message = m_connection.recv_message();
    if (!message) {
        return std::unexpected(message.error());
    }

    if (message->header.response_to != 0 && message->header.response_to != request_id) {
        return std::unexpected(MongoError(MONGO_ERROR_PROTOCOL,
                                          "Unexpected responseTo: " +
                                          std::to_string(message->header.response_to) +
                                          ", expected " + std::to_string(request_id)));
    }

    MongoReply reply(std::move(message->body));
    if (check_ok && !reply.ok()) {
        return std::unexpected(MongoError(MONGO_ERROR_SERVER,
                                          reply.error_code(),
                                          reply.error_message().empty()
                                              ? "Mongo command failed"
                                              : reply.error_message()));
    }

    return std::move(reply);
}

MongoVoidResult MongoClient::authenticate_if_needed(const MongoConfig& config)
{
    if (config.username.empty() && config.password.empty()) {
        return {};
    }

    if (config.username.empty() || config.password.empty()) {
        return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM,
                                          "Both username and password are required for authentication"));
    }

    return authenticate_scram_sha256(config);
}

MongoVoidResult MongoClient::authenticate_scram_sha256(const MongoConfig& config)
{
    const std::string auth_db =
        !config.auth_database.empty() ? config.auth_database :
        (!config.database.empty() ? config.database : "admin");

    auto nonce_or_err = generate_client_nonce();
    if (!nonce_or_err) {
        return std::unexpected(nonce_or_err.error());
    }

    const std::string client_nonce = nonce_or_err.value();
    const std::string client_first_bare =
        "n=" + escape_scram_username(config.username) + ",r=" + client_nonce;
    const std::string client_first_message = "n,," + client_first_bare;

    MongoValue::Binary first_payload(client_first_message.begin(), client_first_message.end());

    MongoDocument sasl_start;
    sasl_start.append("saslStart", int32_t(1));
    sasl_start.append("mechanism", "SCRAM-SHA-256");
    sasl_start.append("payload", std::move(first_payload));
    sasl_start.append("autoAuthorize", int32_t(1));

    auto start_reply = run_command_request(auth_db, sasl_start, true);
    if (!start_reply) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, start_reply.error().message()));
    }

    const auto& start_doc = start_reply->document();
    int32_t conversation_id = start_doc.get_int32("conversationId", 0);
    if (conversation_id == 0) {
        conversation_id = static_cast<int32_t>(start_doc.get_int64("conversationId", 0));
    }
    if (conversation_id == 0) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Missing conversationId in saslStart response"));
    }

    const auto* start_payload_field = start_doc.find("payload");
    if (!start_payload_field || !start_payload_field->is_binary()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Missing payload in saslStart response"));
    }

    const auto& start_payload_binary = start_payload_field->to_binary();
    const std::string server_first_message(start_payload_binary.begin(),
                                           start_payload_binary.end());

    const auto start_kv = parse_scram_payload(server_first_message);
    const auto nonce_it = start_kv.find("r");
    const auto salt_it = start_kv.find("s");
    const auto iter_it = start_kv.find("i");

    if (nonce_it == start_kv.end() || salt_it == start_kv.end() || iter_it == start_kv.end()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Invalid SCRAM server-first-message"));
    }

    const std::string& server_nonce = nonce_it->second;
    if (server_nonce.rfind(client_nonce, 0) != 0) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "SCRAM server nonce does not include client nonce"));
    }

    int iterations = 0;
    const auto parse_iter_result = std::from_chars(iter_it->second.data(),
                                                   iter_it->second.data() + iter_it->second.size(),
                                                   iterations);
    if (parse_iter_result.ec != std::errc{} || iterations <= 0) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Invalid SCRAM iteration count"));
    }

    auto salt_or_err = base64_decode(salt_it->second);
    if (!salt_or_err) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Invalid SCRAM salt: " + salt_or_err.error().message()));
    }

    const std::string client_final_without_proof = "c=biws,r=" + server_nonce;
    const std::string auth_message = client_first_bare + "," +
                                     server_first_message + "," +
                                     client_final_without_proof;

    auto salted_password = pbkdf2_hmac_sha256(config.password, salt_or_err.value(), iterations);
    if (!salted_password) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "PBKDF2 failed: " + salted_password.error().message()));
    }

    auto client_key = hmac_sha256(salted_password.value(), "Client Key");
    if (!client_key) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "HMAC(client key) failed: " + client_key.error().message()));
    }

    auto stored_key = sha256(client_key.value());
    if (!stored_key) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "SHA256(stored key) failed: " + stored_key.error().message()));
    }

    auto client_signature = hmac_sha256(stored_key.value(), auth_message);
    if (!client_signature) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "HMAC(client signature) failed: " +
                                          client_signature.error().message()));
    }

    auto server_key = hmac_sha256(salted_password.value(), "Server Key");
    if (!server_key) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "HMAC(server key) failed: " + server_key.error().message()));
    }

    auto server_signature = hmac_sha256(server_key.value(), auth_message);
    if (!server_signature) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "HMAC(server signature) failed: " +
                                          server_signature.error().message()));
    }

    const auto client_proof = xor_bytes(client_key.value(), client_signature.value());
    const auto expected_server_signature = base64_encode(server_signature.value());

    const std::string client_final_message =
        client_final_without_proof + ",p=" + base64_encode(client_proof);

    MongoValue::Binary continue_payload(client_final_message.begin(), client_final_message.end());

    MongoDocument sasl_continue;
    sasl_continue.append("saslContinue", int32_t(1));
    sasl_continue.append("conversationId", conversation_id);
    sasl_continue.append("payload", std::move(continue_payload));

    auto continue_reply = run_command_request(auth_db, sasl_continue, true);
    if (!continue_reply) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, continue_reply.error().message()));
    }

    const auto& continue_doc = continue_reply->document();
    const auto* continue_payload_field = continue_doc.find("payload");
    if (!continue_payload_field || !continue_payload_field->is_binary()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "Missing payload in saslContinue response"));
    }

    const auto& continue_payload_binary = continue_payload_field->to_binary();
    const std::string server_final_message(continue_payload_binary.begin(),
                                           continue_payload_binary.end());

    const auto final_kv = parse_scram_payload(server_final_message);
    const auto error_it = final_kv.find("e");
    if (error_it != final_kv.end()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "SCRAM server-final-message error: " + error_it->second));
    }

    const auto verifier_it = final_kv.find("v");
    if (verifier_it == final_kv.end()) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "SCRAM server-final-message missing verifier"));
    }

    if (verifier_it->second != expected_server_signature) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                          "SCRAM server signature mismatch"));
    }

    const bool done = continue_doc.get_bool("done", false);
    if (!done) {
        MongoDocument final_continue;
        final_continue.append("saslContinue", int32_t(1));
        final_continue.append("conversationId", conversation_id);
        final_continue.append("payload", MongoValue::Binary{});

        auto final_reply = run_command_request(auth_db, final_continue, true);
        if (!final_reply) {
            return std::unexpected(MongoError(MONGO_ERROR_AUTH, final_reply.error().message()));
        }

        if (!final_reply->document().get_bool("done", false)) {
            return std::unexpected(MongoError(MONGO_ERROR_AUTH,
                                              "SCRAM authentication not finished"));
        }
    }

    return {};
}

std::string MongoClient::escape_scram_username(const std::string& username)
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

std::unordered_map<std::string, std::string>
MongoClient::parse_scram_payload(const std::string& payload)
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

std::string MongoClient::base64_encode(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
        return "";
    }
    return galay::utils::Base64Util::base64_encode(bytes.data(), bytes.size());
}

std::expected<std::vector<uint8_t>, MongoError>
MongoClient::base64_decode(const std::string& text)
{
    if (text.empty()) {
        return std::vector<uint8_t>{};
    }

    if (!galay::utils::Base64Util::base64_can_decode(text)) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, "base64 decode failed"));
    }

    std::string decoded = galay::utils::Base64Util::base64_decode(text);
    return std::vector<uint8_t>(decoded.begin(), decoded.end());
}

std::expected<std::vector<uint8_t>, MongoError>
MongoClient::pbkdf2_hmac_sha256(const std::string& password,
                               const std::vector<uint8_t>& salt,
                               int iterations)
{
    if (iterations <= 0) {
        return std::unexpected(MongoError(MONGO_ERROR_AUTH, "PKCS5_PBKDF2_HMAC failed"));
    }
    return galay::utils::PBKDF2::hmac_sha256(password, salt, static_cast<uint32_t>(iterations), 32);
}

std::expected<std::vector<uint8_t>, MongoError>
MongoClient::hmac_sha256(const std::vector<uint8_t>& key, const std::string& data)
{
    const auto digest = galay::utils::HMAC::hmac_sha256(
        key.data(),
        key.size(),
        reinterpret_cast<const uint8_t*>(data.data()),
        data.size());
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::expected<std::vector<uint8_t>, MongoError>
MongoClient::sha256(const std::vector<uint8_t>& data)
{
    const auto digest = galay::utils::SHA256::hash(data.data(), data.size());
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::vector<uint8_t>
MongoClient::xor_bytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    const size_t size = std::min(a.size(), b.size());
    std::vector<uint8_t> out(size, 0);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return out;
}

std::expected<std::string, MongoError> MongoClient::generate_client_nonce()
{
    std::vector<uint8_t> random_bytes = galay::utils::SaltGenerator::generate_secure_bytes(18);
    return base64_encode(random_bytes);
}

} // namespace galay::mongo
