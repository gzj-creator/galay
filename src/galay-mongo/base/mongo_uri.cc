#include "mongo_uri.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>

namespace galay::mongo
{
namespace
{

constexpr std::string_view kMongoScheme = "mongodb://";

std::unexpected<MongoError> invalidParam(std::string message)
{
    return std::unexpected(MongoError(MONGO_ERROR_INVALID_PARAM, std::move(message)));
}

std::unexpected<MongoError> unsupported(std::string message)
{
    return std::unexpected(MongoError(MONGO_ERROR_UNSUPPORTED, std::move(message)));
}

bool isDigits(std::string_view text)
{
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

std::expected<uint16_t, MongoError> parsePort(std::string_view text)
{
    if (!isDigits(text)) {
        return invalidParam("malformed port");
    }

    uint32_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last || value == 0 || value > 65535) {
        return invalidParam("port out of range");
    }
    return static_cast<uint16_t>(value);
}

std::expected<MongoEndpoint, MongoError> parseEndpoint(std::string_view text)
{
    if (text.empty()) {
        return invalidParam("empty seed");
    }

    MongoEndpoint endpoint;
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        endpoint.host = std::string(text);
        endpoint.port = MongoConfig::kDefaultPort;
    } else {
        if (colon == 0 || colon + 1 >= text.size()) {
            return invalidParam("malformed seed");
        }
        endpoint.host = std::string(text.substr(0, colon));
        auto port = parsePort(text.substr(colon + 1));
        if (!port) {
            return std::unexpected(port.error());
        }
        endpoint.port = *port;
    }

    if (endpoint.host.empty()) {
        return invalidParam("empty seed host");
    }
    return endpoint;
}

std::expected<std::vector<MongoEndpoint>, MongoError> parseSeeds(std::string_view authority)
{
    if (authority.empty()) {
        return invalidParam("empty seed list");
    }

    std::vector<MongoEndpoint> seeds;
    size_t begin = 0;
    while (begin <= authority.size()) {
        const size_t comma = authority.find(',', begin);
        const auto token = authority.substr(
            begin,
            comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        auto endpoint = parseEndpoint(token);
        if (!endpoint) {
            return std::unexpected(endpoint.error());
        }
        seeds.push_back(std::move(*endpoint));
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }

    if (seeds.empty()) {
        return invalidParam("empty seed list");
    }
    return seeds;
}

std::expected<MongoReadPreference, MongoError> parseReadPreference(std::string_view value)
{
    if (value == "primary") {
        return MongoReadPreference::kPrimary;
    }
    if (value == "primaryPreferred") {
        return MongoReadPreference::kPrimaryPreferred;
    }
    if (value == "secondary") {
        return MongoReadPreference::kSecondary;
    }
    if (value == "secondaryPreferred") {
        return MongoReadPreference::kSecondaryPreferred;
    }
    if (value == "nearest") {
        return MongoReadPreference::kNearest;
    }
    return invalidParam("unsupported readPreference");
}

std::expected<uint32_t, MongoError> parseUint32(std::string_view value, std::string_view name)
{
    if (!isDigits(value)) {
        return invalidParam(std::string(name) + " must be numeric");
    }

    uint32_t parsed = 0;
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc() || ptr != last) {
        return invalidParam(std::string(name) + " out of range");
    }
    return parsed;
}

bool isTlsOption(std::string_view key)
{
    return key == "tls" || key == "ssl" || key == "tlsCAFile" ||
           key == "tlsCertificateKeyFile" || key == "tlsAllowInvalidCertificates" ||
           key == "tlsAllowInvalidHostnames";
}

std::expected<void, MongoError> applyQueryOption(MongoConfig& cfg,
                                                 std::string_view key,
                                                 std::string_view value)
{
    if (key.empty()) {
        return invalidParam("empty URI option");
    }

    if (isTlsOption(key)) {
        return unsupported("TLS URI options are not supported yet");
    }

    if (key == "replicaSet") {
        cfg.topology.replica_set_name = std::string(value);
        return {};
    }
    if (key == "authSource") {
        if (value.empty()) {
            return invalidParam("authSource cannot be empty");
        }
        cfg.auth_database = std::string(value);
        return {};
    }
    if (key == "readPreference") {
        auto read_preference = parseReadPreference(value);
        if (!read_preference) {
            return std::unexpected(read_preference.error());
        }
        cfg.topology.read_preference = *read_preference;
        return {};
    }
    if (key == "serverSelectionTimeoutMS") {
        auto timeout = parseUint32(value, key);
        if (!timeout) {
            return std::unexpected(timeout.error());
        }
        cfg.topology.server_selection_timeout = std::chrono::milliseconds(*timeout);
        return {};
    }
    if (key == "connectTimeoutMS") {
        auto timeout = parseUint32(value, key);
        if (!timeout) {
            return std::unexpected(timeout.error());
        }
        cfg.connect_timeout_ms = *timeout;
        return {};
    }
    if (key == "appName") {
        cfg.app_name = std::string(value);
        return {};
    }

    return invalidParam("unsupported URI option: " + std::string(key));
}

std::expected<void, MongoError> applyQuery(MongoConfig& cfg, std::string_view query)
{
    size_t begin = 0;
    while (begin < query.size()) {
        const size_t amp = query.find('&', begin);
        const auto pair = query.substr(
            begin,
            amp == std::string_view::npos ? std::string_view::npos : amp - begin);
        const size_t eq = pair.find('=');
        if (eq == std::string_view::npos) {
            return invalidParam("URI option missing '='");
        }

        auto applied = applyQueryOption(cfg, pair.substr(0, eq), pair.substr(eq + 1));
        if (!applied) {
            return applied;
        }

        if (amp == std::string_view::npos) {
            break;
        }
        begin = amp + 1;
    }
    return {};
}

} // namespace

std::expected<MongoConfig, MongoError> parseMongoUri(std::string_view uri)
{
    if (!uri.starts_with(kMongoScheme)) {
        return invalidParam("URI scheme must be mongodb://");
    }

    MongoConfig cfg;
    std::string_view rest = uri.substr(kMongoScheme.size());
    const size_t query_pos = rest.find('?');
    const std::string_view before_query =
        query_pos == std::string_view::npos ? rest : rest.substr(0, query_pos);
    const std::string_view query =
        query_pos == std::string_view::npos ? std::string_view{} : rest.substr(query_pos + 1);

    const size_t slash = before_query.find('/');
    std::string_view authority =
        slash == std::string_view::npos ? before_query : before_query.substr(0, slash);
    const std::string_view database =
        slash == std::string_view::npos ? std::string_view{} : before_query.substr(slash + 1);

    const size_t at = authority.find('@');
    if (at != std::string_view::npos) {
        const std::string_view credential = authority.substr(0, at);
        const size_t colon = credential.find(':');
        if (colon == std::string_view::npos) {
            return invalidParam("URI credentials must be user:password");
        }
        cfg.username = std::string(credential.substr(0, colon));
        cfg.password = std::string(credential.substr(colon + 1));
        authority = authority.substr(at + 1);
    }

    auto seeds = parseSeeds(authority);
    if (!seeds) {
        return std::unexpected(seeds.error());
    }

    cfg.seeds = std::move(*seeds);
    cfg.host = cfg.seeds.front().host;
    cfg.port = cfg.seeds.front().port;

    if (!database.empty()) {
        cfg.database = std::string(database);
    }

    if (!query.empty()) {
        auto applied = applyQuery(cfg, query);
        if (!applied) {
            return std::unexpected(applied.error());
        }
    }

    return cfg;
}

} // namespace galay::mongo
