#pragma once

#include <serde/reflect/reflect_macros.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Endpoint {
    std::string host;
    int port{};
    bool operator==(const Endpoint&) const = default;
};

#define ENDPOINT_FIELDS(X) X(host) X(port)
REFLECT_FIELDS(Endpoint, ENDPOINT_FIELDS)
#undef ENDPOINT_FIELDS

struct ServiceConfig {
    std::string name;
    std::optional<std::string> description;
    std::vector<Endpoint> endpoints;
    bool enabled{};
    bool operator==(const ServiceConfig&) const = default;
};

#define SERVICE_FIELDS(X) X(name, "service-name") X(description) X(endpoints) X(enabled)
REFLECT_FIELDS(ServiceConfig, SERVICE_FIELDS)
#undef SERVICE_FIELDS

} // namespace

inline int verify_struct_formats() {
    const ServiceConfig original{
        "api", std::nullopt, {{"127.0.0.1", 8080}, {"localhost", 8081}}, true};
    const auto json_text = json::serialize(original);
    assert(json_text);
    assert(json_text->find("\"service-name\"") != std::string::npos);
    const auto from_json = json::deserialize<ServiceConfig>(*json_text);
    assert(from_json && *from_json == original);

    const auto toml_text = toml::serialize(*from_json);
    assert(toml_text);
    assert(toml_text->find("service-name") != std::string::npos);
    const auto from_toml = toml::deserialize<ServiceConfig>(*toml_text);
    assert(from_toml && *from_toml == original);

    assert(!json::deserialize<ServiceConfig>(R"({"enabled":true})"));
    assert(!json::deserialize<Endpoint>(R"({"host":"localhost","port":"invalid"})"));
    assert(!json::deserialize<Endpoint>("{"));
    assert(!toml::deserialize<ServiceConfig>("enabled = true\n"));
    assert(!toml::deserialize<Endpoint>("host = 'localhost'\nport = 'invalid'\n"));
    assert(!toml::deserialize<Endpoint>("["));
    return 0;
}
