#ifndef GALAY_MONGO_TEST_IT_CONFIG_H
#define GALAY_MONGO_TEST_IT_CONFIG_H

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mongo_test
{

struct MongoItEndpoint
{
    std::string host;
    uint16_t port = galay::mongo::MongoConfig::kDefaultPort;
};

struct MongoReplicaSetItConfig
{
    bool enabled = false;
    std::vector<MongoItEndpoint> seeds;
    std::string replica_set_name;
    MongoTestConfig mongo;
    uint32_t server_selection_timeout_ms = 30000;
};

inline bool parseItBool(const char* value, bool fallback = false)
{
    if (value == nullptr) {
        return fallback;
    }

    std::string normalized(value);
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "1" || normalized == "true" ||
           normalized == "yes" || normalized == "on";
}

inline std::optional<MongoItEndpoint> parseItEndpoint(std::string token)
{
    const auto trim = [](std::string& value) {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            value.clear();
            return;
        }
        const auto end = value.find_last_not_of(" \t\r\n");
        value = value.substr(begin, end - begin + 1);
    };

    trim(token);
    if (token.empty()) {
        return std::nullopt;
    }

    const auto colon = token.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
        return std::nullopt;
    }

    MongoItEndpoint endpoint;
    endpoint.host = token.substr(0, colon);
    const std::string port_text = token.substr(colon + 1);

    try {
        const int port = std::stoi(port_text);
        if (port <= 0 || port > 65535) {
            return std::nullopt;
        }
        endpoint.port = static_cast<uint16_t>(port);
    } catch (...) {
        return std::nullopt;
    }

    return endpoint;
}

inline std::vector<MongoItEndpoint> parseItSeedList(const char* value)
{
    std::vector<MongoItEndpoint> seeds;
    if (value == nullptr) {
        return seeds;
    }

    std::stringstream stream(value);
    std::string token;
    while (std::getline(stream, token, ',')) {
        auto endpoint = parseItEndpoint(std::move(token));
        if (endpoint) {
            seeds.push_back(std::move(*endpoint));
        }
    }
    return seeds;
}

inline MongoReplicaSetItConfig loadMongoReplicaSetItConfig()
{
    MongoReplicaSetItConfig cfg;
    cfg.enabled = parseItBool(std::getenv("GALAY_IT_ENABLE"), false);
    cfg.seeds = parseItSeedList(std::getenv("GALAY_MONGO_RS_SEEDS"));
    cfg.replica_set_name = envOrDefault("GALAY_MONGO_RS_NAME", "");
    cfg.mongo = loadMongoTestConfig();
    cfg.server_selection_timeout_ms =
        envUint32OrDefault("GALAY_MONGO_SERVER_SELECTION_TIMEOUT_MS",
                           cfg.server_selection_timeout_ms);
    return cfg;
}

inline bool shouldSkipReplicaSetIt(const MongoReplicaSetItConfig& cfg, std::string* reason)
{
    if (!cfg.enabled) {
        if (reason != nullptr) {
            *reason = "set GALAY_IT_ENABLE=1";
        }
        return true;
    }

    if (cfg.seeds.empty()) {
        if (reason != nullptr) {
            *reason = "set GALAY_MONGO_RS_SEEDS=host1:port1,host2:port2";
        }
        return true;
    }

    if (cfg.replica_set_name.empty()) {
        if (reason != nullptr) {
            *reason = "set GALAY_MONGO_RS_NAME";
        }
        return true;
    }

    return false;
}

} // namespace mongo_test

#endif // GALAY_MONGO_TEST_IT_CONFIG_H
