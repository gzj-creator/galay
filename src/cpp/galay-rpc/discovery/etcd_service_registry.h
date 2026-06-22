/**
 * @file etcd_service_registry.h
 * @brief RPC etcd注册中心适配接口
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 默认提供无外部依赖的fake contract；当构建同时启用 galay-etcd 时，
 *          EtcdServiceRegistry 使用真实 etcd KV 作为服务注册中心。
 */

#ifndef GALAY_RPC_ETCD_SERVICE_REGISTRY_H
#define GALAY_RPC_ETCD_SERVICE_REGISTRY_H

#include "../kernel/rpc_endpoint_cache.h"
#include "../protoc/rpc_error.h"

#ifdef GALAY_RPC_HAS_ETCD
#include "../../galay-etcd/sync/etcd_client.h"
#endif

#include <charconv>
#include <chrono>
#include <cctype>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::rpc
{

struct RpcEtcdRegistryConfig {
    std::string endpoint;  ///< etcd HTTP endpoint，例如 http://127.0.0.1:2379
    std::string api_prefix = "/v3";  ///< etcd v3 REST API 前缀
    std::string prefix = "/galay/rpc";  ///< 默认注册根路径
    /**
     * @brief endpoint key模板
     * @details 支持 `{prefix}`、`{service}`、`{instance}` 占位符。discover 使用
     *          `{instance}` 之前的模板片段作为前缀查询范围。
     */
    std::string key_template = "{prefix}/{service}/{instance}";
    std::chrono::milliseconds request_timeout{3000};  ///< etcd 单次请求超时
};

class FakeEtcdServiceRegistry {
public:
    explicit FakeEtcdServiceRegistry(std::string prefix)
        : m_prefix(std::move(prefix))
    {
    }

    std::expected<void, RpcError> registerEndpoint(const RpcEndpointInfo& endpoint) {
        auto& endpoints = m_services[endpoint.service];
        auto it = std::find_if(endpoints.begin(), endpoints.end(), [&](const RpcEndpointInfo& item) {
            return item.instance_id == endpoint.instance_id;
        });
        if (it == endpoints.end()) {
            endpoints.push_back(endpoint);
        } else {
            *it = endpoint;
        }
        notify(endpoint.service, RpcEndpointEvent::update(endpoint));
        return {};
    }

    std::expected<void, RpcError> deregisterEndpoint(const std::string& service,
                                                     const std::string& instance_id) {
        auto& endpoints = m_services[service];
        std::erase_if(endpoints, [&](const RpcEndpointInfo& item) {
            return item.instance_id == instance_id;
        });
        notify(service, RpcEndpointEvent::remove(service, instance_id));
        return {};
    }

    std::expected<std::vector<RpcEndpointInfo>, RpcError> discover(const std::string& service) const {
        auto it = m_services.find(service);
        if (it == m_services.end()) {
            return std::vector<RpcEndpointInfo>{};
        }
        return it->second;
    }

    std::expected<void, RpcError> watch(const std::string& service,
                                        RpcEndpointCache& cache,
                                        std::function<void()> callback = {}) {
        m_watchers[service].push_back(Watcher{&cache, std::move(callback)});
        return {};
    }

private:
    struct Watcher {
        RpcEndpointCache* cache = nullptr;
        std::function<void()> callback;
    };

    void notify(const std::string& service, const RpcEndpointEvent& event) {
        auto it = m_watchers.find(service);
        if (it == m_watchers.end()) {
            return;
        }
        for (auto& watcher : it->second) {
            if (watcher.cache != nullptr) {
                watcher.cache->apply(event);
            }
            if (watcher.callback) {
                watcher.callback();
            }
        }
    }

    std::string m_prefix;
    std::unordered_map<std::string, std::vector<RpcEndpointInfo>> m_services;
    std::unordered_map<std::string, std::vector<Watcher>> m_watchers;
};

class EtcdServiceRegistry {
public:
    explicit EtcdServiceRegistry(RpcEtcdRegistryConfig config)
        : m_config(std::move(config))
    {
    }

    std::expected<void, RpcError> integrationAvailable() const {
        if (m_config.endpoint.empty()) {
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE,
                                            "GALAY_ETCD_ENDPOINT is empty"));
        }
#ifdef GALAY_RPC_HAS_ETCD
        auto client = makeClient();
        auto connected = client.connect();
        if (!connected.has_value()) {
            return std::unexpected(fromEtcdError("etcd connect", connected.error()));
        }
        if (!connected.value()) {
            return std::unexpected(RpcError(RpcErrorCode::UNAVAILABLE,
                                            "etcd connect returned false"));
        }
        return {};
#else
        return std::unexpected(noEtcdSupport());
#endif
    }

    /**
     * @brief 注册或更新一个 endpoint 到 etcd。
     * @return 成功或 etcd/RPC 参数错误。
     */
    std::expected<void, RpcError> registerEndpoint(const RpcEndpointInfo& endpoint) {
        auto validated = validateEndpoint(endpoint);
        if (!validated.has_value()) {
            return std::unexpected(validated.error());
        }
        auto key = keyFor(endpoint.service, endpoint.instance_id);
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
#ifdef GALAY_RPC_HAS_ETCD
        auto client = makeClient();
        auto connected = client.connect();
        if (!connected.has_value()) {
            return std::unexpected(fromEtcdError("etcd connect", connected.error()));
        }
        auto stored = client.put(*key, encodeEndpoint(endpoint));
        if (!stored.has_value()) {
            return std::unexpected(fromEtcdError("etcd put", stored.error()));
        }
        notify(endpoint.service, RpcEndpointEvent::update(endpoint));
        return {};
#else
        return std::unexpected(noEtcdSupport());
#endif
    }

    /**
     * @brief 从 etcd 删除一个 endpoint。
     * @return 删除请求成功或 etcd/RPC 参数错误；不存在的 key 视为成功。
     */
    std::expected<void, RpcError> deregisterEndpoint(const std::string& service,
                                                     const std::string& instance_id) {
        auto key = keyFor(service, instance_id);
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
#ifdef GALAY_RPC_HAS_ETCD
        auto client = makeClient();
        auto connected = client.connect();
        if (!connected.has_value()) {
            return std::unexpected(fromEtcdError("etcd connect", connected.error()));
        }
        auto deleted = client.del(*key);
        if (!deleted.has_value()) {
            return std::unexpected(fromEtcdError("etcd delete", deleted.error()));
        }
        notify(service, RpcEndpointEvent::remove(service, instance_id));
        return {};
#else
        return std::unexpected(noEtcdSupport());
#endif
    }

    /**
     * @brief 按服务名前缀查询 etcd endpoint。
     * @return 可解析的 endpoint 列表。单个脏值会被跳过，避免影响同服务健康实例。
     */
    std::expected<std::vector<RpcEndpointInfo>, RpcError> discover(const std::string& service) const {
        auto prefix = servicePrefix(service);
        if (!prefix.has_value()) {
            return std::unexpected(prefix.error());
        }
#ifdef GALAY_RPC_HAS_ETCD
        auto client = makeClient();
        auto connected = client.connect();
        if (!connected.has_value()) {
            return std::unexpected(fromEtcdError("etcd connect", connected.error()));
        }
        auto values = client.get(*prefix, true);
        if (!values.has_value()) {
            return std::unexpected(fromEtcdError("etcd get", values.error()));
        }
        std::vector<RpcEndpointInfo> endpoints;
        for (const auto& item : *values) {
            auto parsed = decodeEndpoint(item.value);
            if (!parsed.has_value() || parsed->service != service) {
                continue;
            }
            endpoints.push_back(std::move(*parsed));
        }
        return endpoints;
#else
        return std::unexpected(noEtcdSupport());
#endif
    }

    /**
     * @brief 注册本进程内的 endpoint 变更回调。
     * @note 当前不启动跨进程 etcd watch 流；register/deregister 会通知本对象回调。
     */
    std::expected<void, RpcError> watch(const std::string& service,
                                        RpcEndpointCache& cache,
                                        std::function<void()> callback = {}) {
        m_watchers[service].push_back(Watcher{&cache, std::move(callback)});
        return {};
    }

private:
    struct Watcher {
        RpcEndpointCache* cache = nullptr;
        std::function<void()> callback;
    };

    static std::expected<void, RpcError> validateEndpoint(const RpcEndpointInfo& endpoint) {
        if (endpoint.host.empty() || endpoint.port == 0 || endpoint.service.empty() ||
            endpoint.instance_id.empty()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC endpoint is missing host, port, service, or instance_id"));
        }
        return {};
    }

    static bool isHex(char ch) {
        return std::isdigit(static_cast<unsigned char>(ch)) ||
               (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    }

    static int hexValue(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        return 10 + ch - 'A';
    }

    static char hexDigit(unsigned value) {
        return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10));
    }

    static std::string escapeValue(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (unsigned char ch : value) {
            if (ch == '%' || ch == '\n' || ch == '\r' || ch == '=') {
                escaped.push_back('%');
                escaped.push_back(hexDigit((ch >> 4U) & 0xFU));
                escaped.push_back(hexDigit(ch & 0xFU));
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
        }
        return escaped;
    }

    static std::expected<std::string, RpcError> unescapeValue(std::string_view value) {
        std::string unescaped;
        unescaped.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '%') {
                unescaped.push_back(value[i]);
                continue;
            }
            if (i + 2 >= value.size() || !isHex(value[i + 1]) || !isHex(value[i + 2])) {
                return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                                "invalid escaped endpoint value"));
            }
            const auto decoded = static_cast<char>((hexValue(value[i + 1]) << 4) |
                                                   hexValue(value[i + 2]));
            unescaped.push_back(decoded);
            i += 2;
        }
        return unescaped;
    }

    static std::string escapeKeyComponent(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (unsigned char ch : value) {
            if (ch == '/' || ch == '%' || std::iscntrl(ch)) {
                escaped.push_back('%');
                escaped.push_back(hexDigit((ch >> 4U) & 0xFU));
                escaped.push_back(hexDigit(ch & 0xFU));
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
        }
        return escaped;
    }

    static std::string statusName(RpcEndpointStatus status) {
        switch (status) {
            case RpcEndpointStatus::Serving:
                return "Serving";
            case RpcEndpointStatus::Draining:
                return "Draining";
            case RpcEndpointStatus::Unavailable:
                return "Unavailable";
        }
        return "Unavailable";
    }

    static std::expected<RpcEndpointStatus, RpcError> parseStatus(std::string_view value) {
        if (value == "Serving") return RpcEndpointStatus::Serving;
        if (value == "Draining") return RpcEndpointStatus::Draining;
        if (value == "Unavailable") return RpcEndpointStatus::Unavailable;
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                        "invalid endpoint status"));
    }

    static bool parseUnsigned(std::string_view value, uint64_t& out) {
        const char* begin = value.data();
        const char* end = begin + value.size();
        auto [ptr, ec] = std::from_chars(begin, end, out);
        return ec == std::errc() && ptr == end;
    }

    static std::string encodeEndpoint(const RpcEndpointInfo& endpoint) {
        std::string value;
        value += "host=" + escapeValue(endpoint.host) + "\n";
        value += "port=" + std::to_string(endpoint.port) + "\n";
        value += "service=" + escapeValue(endpoint.service) + "\n";
        value += "instance_id=" + escapeValue(endpoint.instance_id) + "\n";
        value += "weight=" + std::to_string(endpoint.weight) + "\n";
        value += "status=" + statusName(endpoint.status) + "\n";
        value += "version=" + escapeValue(endpoint.version) + "\n";
        value += "zone=" + escapeValue(endpoint.zone) + "\n";
        for (const auto& [key, item] : endpoint.metadata) {
            value += "metadata." + escapeValue(key) + "=" + escapeValue(item) + "\n";
        }
        return value;
    }

    static std::expected<RpcEndpointInfo, RpcError> decodeEndpoint(std::string_view value) {
        RpcEndpointInfo endpoint;
        size_t cursor = 0;
        while (cursor <= value.size()) {
            size_t line_end = value.find('\n', cursor);
            if (line_end == std::string_view::npos) {
                line_end = value.size();
            }
            auto line = value.substr(cursor, line_end - cursor);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!line.empty()) {
                const size_t equals = line.find('=');
                if (equals == std::string_view::npos) {
                    return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                                    "malformed endpoint value"));
                }
                const auto key = line.substr(0, equals);
                auto decoded = unescapeValue(line.substr(equals + 1));
                if (!decoded.has_value()) {
                    return std::unexpected(decoded.error());
                }
                if (key == "host") {
                    endpoint.host = std::move(*decoded);
                } else if (key == "port") {
                    uint64_t parsed = 0;
                    if (!parseUnsigned(*decoded, parsed) || parsed > 65535) {
                        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                                        "invalid endpoint port"));
                    }
                    endpoint.port = static_cast<uint16_t>(parsed);
                } else if (key == "service") {
                    endpoint.service = std::move(*decoded);
                } else if (key == "instance_id") {
                    endpoint.instance_id = std::move(*decoded);
                } else if (key == "weight") {
                    uint64_t parsed = 0;
                    if (!parseUnsigned(*decoded, parsed) ||
                        parsed > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                                        "invalid endpoint weight"));
                    }
                    endpoint.weight = static_cast<uint32_t>(parsed);
                } else if (key == "status") {
                    auto status = parseStatus(*decoded);
                    if (!status.has_value()) {
                        return std::unexpected(status.error());
                    }
                    endpoint.status = *status;
                } else if (key == "version") {
                    endpoint.version = std::move(*decoded);
                } else if (key == "zone") {
                    endpoint.zone = std::move(*decoded);
                } else if (key.starts_with("metadata.")) {
                    auto metadata_key = unescapeValue(key.substr(std::string_view("metadata.").size()));
                    if (!metadata_key.has_value()) {
                        return std::unexpected(metadata_key.error());
                    }
                    endpoint.metadata.emplace(std::move(*metadata_key), std::move(*decoded));
                }
            }
            if (line_end == value.size()) {
                break;
            }
            cursor = line_end + 1;
        }
        auto validated = validateEndpoint(endpoint);
        if (!validated.has_value()) {
            return std::unexpected(validated.error());
        }
        return endpoint;
    }

    static void replaceAll(std::string& value, std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = value.find(from, pos)) != std::string::npos) {
            value.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string normalizeKey(std::string key) {
        std::string normalized;
        normalized.reserve(key.size() + 1);
        bool last_slash = false;
        for (char ch : key) {
            if (ch == '/') {
                if (!last_slash) {
                    normalized.push_back(ch);
                }
                last_slash = true;
            } else {
                normalized.push_back(ch);
                last_slash = false;
            }
        }
        if (normalized.empty() || normalized.front() != '/') {
            normalized.insert(normalized.begin(), '/');
        }
        return normalized;
    }

    static std::expected<void, RpcError> validateKeyTemplate(std::string_view key_template) {
        if (key_template.find("{prefix}") == std::string_view::npos ||
            key_template.find("{service}") == std::string_view::npos ||
            key_template.find("{instance}") == std::string_view::npos) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "etcd key_template must contain {prefix}, {service}, and {instance}"));
        }
        return {};
    }

    std::expected<std::string, RpcError> keyFor(const std::string& service,
                                                const std::string& instance_id) const {
        auto valid = validateKeyTemplate(m_config.key_template);
        if (!valid.has_value()) {
            return std::unexpected(valid.error());
        }
        std::string key = m_config.key_template;
        replaceAll(key, "{prefix}", m_config.prefix);
        replaceAll(key, "{service}", escapeKeyComponent(service));
        replaceAll(key, "{instance}", escapeKeyComponent(instance_id));
        return normalizeKey(std::move(key));
    }

    std::expected<std::string, RpcError> servicePrefix(const std::string& service) const {
        auto valid = validateKeyTemplate(m_config.key_template);
        if (!valid.has_value()) {
            return std::unexpected(valid.error());
        }
        const size_t instance_pos = m_config.key_template.find("{instance}");
        std::string prefix_template = m_config.key_template.substr(0, instance_pos);
        replaceAll(prefix_template, "{prefix}", m_config.prefix);
        replaceAll(prefix_template, "{service}", escapeKeyComponent(service));
        return normalizeKey(std::move(prefix_template));
    }

#ifdef GALAY_RPC_HAS_ETCD
    galay::etcd::EtcdClient makeClient() const {
        galay::etcd::EtcdConfig config;
        config.endpoint = m_config.endpoint;
        config.api_prefix = m_config.api_prefix;
        config.request_timeout = m_config.request_timeout;
        return galay::etcd::EtcdClientBuilder().config(std::move(config)).build();
    }

    static RpcError fromEtcdError(const char* operation, const galay::etcd::EtcdError& error) {
        return RpcError(RpcErrorCode::UNAVAILABLE,
                        std::string(operation) + " failed: " + error.message());
    }
#endif

    static RpcError noEtcdSupport() {
        return RpcError(RpcErrorCode::UNAVAILABLE,
                        "galay-rpc was built without galay-etcd support");
    }

    void notify(const std::string& service, const RpcEndpointEvent& event) {
        auto it = m_watchers.find(service);
        if (it == m_watchers.end()) {
            return;
        }
        for (auto& watcher : it->second) {
            if (watcher.cache != nullptr) {
                watcher.cache->apply(event);
            }
            if (watcher.callback) {
                watcher.callback();
            }
        }
    }

    RpcEtcdRegistryConfig m_config;
    std::unordered_map<std::string, std::vector<Watcher>> m_watchers;
};

} // namespace galay::rpc

#endif // GALAY_RPC_ETCD_SERVICE_REGISTRY_H
