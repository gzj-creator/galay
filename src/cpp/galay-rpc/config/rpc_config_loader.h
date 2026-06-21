/**
 * @file rpc_config_loader.h
 * @brief RPC本地配置加载器
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供小型TOML子集加载器，只解析文档化的RPC配置键。
 */

#ifndef GALAY_RPC_CONFIG_LOADER_H
#define GALAY_RPC_CONFIG_LOADER_H

#include "../kernel/rpc_config.h"

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace galay::rpc
{

enum class RpcConfigErrorCode {
    MissingFile,
    Malformed,
    InvalidValue
};

struct RpcConfigError {
    RpcConfigErrorCode code = RpcConfigErrorCode::Malformed;
    std::string message;
};

namespace detail {

inline std::string trim(std::string value)
{
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

inline std::expected<std::string, RpcConfigError> parseString(std::string value)
{
    value = trim(std::move(value));
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "expected string"});
    }
    return value.substr(1, value.size() - 2);
}

inline std::expected<int64_t, RpcConfigError> parseInt(std::string value)
{
    value = trim(std::move(value));
    if (value.empty()) {
        return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "empty integer"});
    }
    size_t pos = 0;
    try {
        const auto parsed = std::stoll(value, &pos);
        if (pos != value.size()) {
            return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "invalid integer"});
        }
        return parsed;
    } catch (...) {
        return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "invalid integer"});
    }
}

inline std::expected<bool, RpcConfigError> parseBool(std::string value)
{
    value = trim(std::move(value));
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "invalid bool"});
}

inline std::string stripComment(std::string line)
{
    bool in_string = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            in_string = !in_string;
        }
        if (!in_string && line[i] == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

} // namespace detail

inline std::expected<RpcConfig, RpcConfigError> LoadRpcConfig(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::unexpected(RpcConfigError{RpcConfigErrorCode::MissingFile, "config file missing"});
    }

    RpcConfig config;
    std::string section;
    std::string line;
    size_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        line = detail::trim(detail::stripComment(std::move(line)));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']') {
                return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "malformed section"});
            }
            section = line.substr(1, line.size() - 2);
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed, "missing equals"});
        }
        const auto key = detail::trim(line.substr(0, equals));
        const auto value = line.substr(equals + 1);

        auto parse_positive = [&](const char* name) -> std::expected<int64_t, RpcConfigError> {
            auto parsed = detail::parseInt(value);
            if (!parsed.has_value()) {
                return parsed;
            }
            if (*parsed <= 0) {
                return std::unexpected(RpcConfigError{RpcConfigErrorCode::InvalidValue, name});
            }
            return parsed;
        };

        if (section == "server" && key == "host") {
            auto parsed = detail::parseString(value);
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.server.host = *parsed;
        } else if (section == "server" && key == "port") {
            auto parsed = parse_positive("server.port");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            if (*parsed > 65535) {
                return std::unexpected(RpcConfigError{RpcConfigErrorCode::InvalidValue, "server.port"});
            }
            config.server.port = static_cast<uint16_t>(*parsed);
        } else if (section == "client" && key == "default_timeout_ms") {
            auto parsed = parse_positive("client.default_timeout_ms");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.client.default_timeout = std::chrono::milliseconds(*parsed);
        } else if (section == "pool" && key == "max_connections_per_endpoint") {
            auto parsed = parse_positive("pool.max_connections_per_endpoint");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.pool.max_connections_per_endpoint = static_cast<size_t>(*parsed);
        } else if (section == "retry" && key == "max_attempts") {
            auto parsed = parse_positive("retry.max_attempts");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.retry.max_attempts = static_cast<uint32_t>(*parsed);
        } else if (section == "retry" && key == "require_idempotent") {
            auto parsed = detail::parseBool(value);
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.retry.require_idempotent = *parsed;
        } else if (section == "governance.rate_limit" && key == "enabled") {
            auto parsed = detail::parseBool(value);
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.governance.rate_limit.enabled = *parsed;
        } else if (section == "governance.rate_limit" && key == "capacity") {
            auto parsed = parse_positive("governance.rate_limit.capacity");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.governance.rate_limit.capacity = static_cast<size_t>(*parsed);
        } else if (section == "discovery" && key == "kind") {
            auto parsed = detail::parseString(value);
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            if (*parsed == "local") {
                config.discovery.kind = RpcDiscoveryKind::Local;
            } else if (*parsed == "etcd") {
                config.discovery.kind = RpcDiscoveryKind::Etcd;
            } else {
                return std::unexpected(RpcConfigError{RpcConfigErrorCode::InvalidValue, "discovery.kind"});
            }
        } else if (section == "discovery" && key == "prefix") {
            auto parsed = detail::parseString(value);
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.discovery.prefix = *parsed;
        } else if (section == "stream" && key == "max_frame_bytes") {
            auto parsed = parse_positive("stream.max_frame_bytes");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.stream.max_frame_bytes = static_cast<size_t>(*parsed);
        } else if (section == "benchmark" && key == "requests") {
            auto parsed = parse_positive("benchmark.requests");
            if (!parsed.has_value()) return std::unexpected(parsed.error());
            config.benchmark.requests = static_cast<size_t>(*parsed);
        } else {
            return std::unexpected(RpcConfigError{RpcConfigErrorCode::Malformed,
                                                 "unknown key at line " + std::to_string(line_no)});
        }
    }

    return config;
}

} // namespace galay::rpc

#endif // GALAY_RPC_CONFIG_LOADER_H
