#include <serde/json/json.hpp>
#include <serde/toml/toml.hpp>

#include <iostream>
#include <string>
#include <vector>

struct Endpoint {
    std::string host;
    int port{};
    bool operator==(const Endpoint&) const = default;
};

struct ServerConfig {
    Endpoint primary;
    std::vector<Endpoint> replicas;
    bool enabled{};
    bool operator==(const ServerConfig&) const = default;
};

int main() {
    const ServerConfig server{{"127.0.0.1", 8080}, {{"127.0.0.1", 8081}}, true};

    const auto json_text = json::serialize(server);
    if (!json_text) {
        std::cerr << json_text.error() << '\n';
        return 1;
    }
    const auto toml_text = toml::serialize(server);
    if (!toml_text) {
        std::cerr << toml_text.error() << '\n';
        return 1;
    }
    std::cout << "JSON\n" << *json_text << "\n\nTOML\n" << *toml_text << '\n';

    const auto from_json = json::deserialize<ServerConfig>(*json_text);
    if (!from_json) {
        std::cerr << from_json.error() << '\n';
        return 1;
    }
    const auto from_toml = toml::deserialize<ServerConfig>(*toml_text);
    if (!from_toml) {
        std::cerr << from_toml.error() << '\n';
        return 1;
    }
    return *from_json == server && *from_toml == server ? 0 : 1;
}
