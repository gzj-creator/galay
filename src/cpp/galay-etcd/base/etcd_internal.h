/**
 * @file etcd_internal.h
 * @brief etcd 客户端内部工具函数集
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 提供 etcd 客户端内部使用的辅助函数，包括：
 *          - JSON 解析与字段提取（基于 simdjson）
 *          - Base64 编解码
 *          - 端点地址解析
 *          - etcd v3 REST API 请求体构建
 *          - etcd v3 REST API 响应体解析
 *          - Watch 事件解析
 *          这些函数仅供内部实现使用，不对外暴露。
 */

#ifndef GALAY_ETCD_INTERNAL_H
#define GALAY_ETCD_INTERNAL_H

#include "etcd_error.h"
#include "etcd_types.h"
#include "etcd_value.h"

#include "../../galay-utils/encoding/base64.hpp"
#include <simdjson.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::etcd::internal
{

/**
 * @brief 将字符串视图解析为有符号 64 位整数
 * @param value 待解析的字符串视图
 * @return 解析成功返回整数值，失败返回 std::nullopt
 */
inline std::optional<int64_t> parseSignedInt(std::string_view value)
{
    if (value.empty()) {
        return std::nullopt;
    }
    int64_t parsed = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

/**
 * @brief 从 simdjson 元素中提取 64 位整数
 * @details 依次尝试 int64、uint64 和字符串三种类型，
 *          若为 uint64 则检查是否在 int64 范围内。
 * @param element simdjson JSON 元素
 * @return 成功提取返回整数值，失败返回 std::nullopt
 */
inline std::optional<int64_t> asInt64(const simdjson::dom::element& element)
{
    auto int64_result = element.get_int64();
    if (!int64_result.error()) {
        return int64_result.value_unsafe();
    }

    auto uint64_result = element.get_uint64();
    if (!uint64_result.error()) {
        const uint64_t value = uint64_result.value_unsafe();
        if (value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(value);
        }
    }

    auto string_result = element.get_string();
    if (!string_result.error()) {
        return parseSignedInt(string_result.value_unsafe());
    }
    return std::nullopt;
}

/**
 * @brief 从 JSON 对象中查找并提取整数字段
 * @param object JSON 对象
 * @param field 字段名
 * @return 成功提取返回整数值，字段不存在或类型不匹配返回 std::nullopt
 */
inline std::optional<int64_t> findIntField(const simdjson::dom::object& object, std::string_view field)
{
    auto field_result = object[field];
    if (field_result.error()) {
        return std::nullopt;
    }
    return asInt64(field_result.value_unsafe());
}

/**
 * @brief 从 JSON 对象中查找并提取字符串字段
 * @param object JSON 对象
 * @param field 字段名
 * @return 成功提取返回字符串值，字段不存在或类型不匹配返回 std::nullopt
 */
inline std::optional<std::string> findStringField(const simdjson::dom::object& object, std::string_view field)
{
    auto field_result = object[field];
    if (field_result.error()) {
        return std::nullopt;
    }

    auto string_result = field_result.value_unsafe().get_string();
    if (string_result.error()) {
        return std::nullopt;
    }
    return std::string(string_result.value_unsafe());
}

/**
 * @brief 创建 JSON 解析错误
 * @param context 错误上下文描述
 * @param error simdjson 错误码
 * @return 包含上下文和错误描述的 EtcdError
 */
inline EtcdError makeJsonParseError(const std::string& context, simdjson::error_code error)
{
    return EtcdError(
        EtcdErrorType::Parse,
        context + ": " + std::string(simdjson::error_message(error)));
}

/**
 * @brief 将字符串解析为 JSON 对象
 * @param body JSON 字符串
 * @param parser JSON 解析器
 * @param parse_error 输出参数，解析失败时写入错误信息（可为 nullptr）
 * @param context 错误上下文描述
 * @return 成功返回 JSON 对象，失败返回 std::nullopt
 */
inline std::optional<simdjson::dom::object> parseJsonObject(
    const std::string& body,
    simdjson::dom::parser& parser,
    EtcdError* parse_error,
    const std::string& context)
{
    auto doc_result = parser.parse(body);
    if (doc_result.error()) {
        if (parse_error != nullptr) {
            *parse_error = makeJsonParseError(context, doc_result.error());
        }
        return std::nullopt;
    }

    auto object_result = doc_result.value_unsafe().get_object();
    if (object_result.error()) {
        if (parse_error != nullptr) {
            *parse_error = makeJsonParseError(context, object_result.error());
        }
        return std::nullopt;
    }

    return object_result.value_unsafe();
}

/**
 * @brief 规范化 API 路径前缀
 * @details 确保前缀以 '/' 开头且不以 '/' 结尾，空字符串默认为 "/v3"。
 * @param prefix 原始前缀字符串
 * @return 规范化后的前缀字符串
 */
inline std::string normalizeApiPrefix(std::string prefix)
{
    if (prefix.empty()) {
        return "/v3";
    }
    if (prefix.front() != '/') {
        prefix.insert(prefix.begin(), '/');
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.pop_back();
    }
    return prefix;
}

/**
 * @brief 计算 etcd 前缀查询的范围结束键
 * @details 根据 etcd 的范围查询语义，计算给定键之后的前缀范围结束键。
 *          从右向左找到第一个小于 0xFF 的字节并加 1，截断该字节之后的内容。
 * @param key 前缀键
 * @return 范围结束键；若所有字节均为 0xFF 则返回单个 '\0'
 */
inline std::string makePrefixRangeEnd(std::string key)
{
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(key.size()) - 1; i >= 0; --i) {
        const unsigned char ch = static_cast<unsigned char>(key[static_cast<size_t>(i)]);
        if (ch < 0xFF) {
            key[static_cast<size_t>(i)] = static_cast<char>(ch + 1);
            key.resize(static_cast<size_t>(i) + 1);
            return key;
        }
    }
    return std::string(1, '\0');
}

/**
 * @brief 将数据编码为 Base64 字符串
 * @param data 待编码的数据视图
 * @return Base64 编码后的字符串
 */
inline std::string encodeBase64(std::string_view data)
{
    return galay::utils::Base64Util::Base64EncodeView(data);
}

/**
 * @brief 将 Base64 字符串解码为原始数据
 * @param data Base64 编码的字符串视图
 * @return 解码成功返回解码后的字符串，解码失败返回 std::nullopt
 */
inline std::optional<std::string> decodeBase64(std::string_view data)
{
    try {
        return galay::utils::Base64Util::Base64DecodeView(data);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (...) {
        return std::nullopt;
    }
}

/**
 * @brief 端点地址解析结果
 * @details 存储 http/https 端点地址的解析结果，
 *          包含主机名、端口、是否为安全连接以及是否为 IPv6 地址。
 */
struct ParsedEndpoint
{
    std::string host;      ///< 主机名或 IP 地址
    uint16_t port = 0;     ///< 端口号
    bool secure = false;   ///< 是否为 HTTPS 连接
    bool ipv6 = false;     ///< 是否为 IPv6 地址
};

/**
 * @brief 解析 etcd 端点地址
 * @details 支持格式：http(s)://host:port、http(s)://[ipv6]:port，
 *          未指定端口时 http 默认 80，https 默认 443。
 * @param endpoint 端点地址字符串
 * @return 解析成功返回 ParsedEndpoint，格式错误返回错误消息
 */
inline std::expected<ParsedEndpoint, std::string> parseEndpoint(const std::string& endpoint)
{
    thread_local const std::regex kEndpointRegex(
        R"(^(http|https)://(\[[^\]]+\]|[^/:]+)(?::(\d+))?(?:/.*)?$)",
        std::regex::icase);
    std::smatch matches;
    if (!std::regex_match(endpoint, matches, kEndpointRegex)) {
        return std::unexpected("invalid endpoint: " + endpoint);
    }

    ParsedEndpoint parsed;
    std::string scheme = matches[1].str();
    std::transform(
        scheme.begin(),
        scheme.end(),
        scheme.begin(),
        [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    parsed.secure = scheme == "https";

    std::string host = matches[2].str();
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
        parsed.ipv6 = true;
    } else if (host.find(':') != std::string::npos) {
        parsed.ipv6 = true;
    }
    parsed.host = std::move(host);

    int port = parsed.secure ? 443 : 80;
    if (matches[3].matched) {
        try {
            port = std::stoi(matches[3].str());
        } catch (const std::bad_alloc&) {
            throw;
        } catch (...) {
            return std::unexpected("invalid endpoint port: " + endpoint);
        }
        if (port <= 0 || port > 65535) {
            return std::unexpected("endpoint port out of range: " + endpoint);
        }
    }
    parsed.port = static_cast<uint16_t>(port);
    return parsed;
}

/**
 * @brief 构建 HTTP Host 头
 * @details 根据 IPv4/IPv6 格式化主机名和端口为 Host 头字符串，
 *          IPv6 地址会被方括号包裹。
 * @param host 主机名
 * @param port 端口号
 * @param ipv6 是否为 IPv6 地址
 * @return 格式化的 Host 头字符串
 */
inline std::string buildHostHeader(const std::string& host, uint16_t port, bool ipv6)
{
    if (ipv6) {
        return "[" + host + "]:" + std::to_string(port);
    }
    return host + ":" + std::to_string(port);
}

/**
 * @brief 获取线程本地的 JSON 解析器实例
 * @details 返回 thread_local 的 simdjson 解析器，避免频繁创建和销毁解析器。
 * @return 线程本地的 simdjson 解析器引用
 */
inline simdjson::dom::parser& threadLocalJsonParser()
{
    thread_local simdjson::dom::parser parser;
    return parser;
}

/**
 * @brief 解析 etcd 成功响应的 JSON 对象
 * @details 解析 JSON 响应体并检查是否包含 etcd 服务端错误（code != 0），
 *          若存在服务端错误则返回 Server 类型的 EtcdError。
 * @param body JSON 响应体字符串
 * @param context 错误上下文描述
 * @return 成功返回 JSON 对象，失败返回 EtcdError
 */
inline std::expected<simdjson::dom::object, EtcdError> parseEtcdSuccessObject(
    const std::string& body,
    const std::string& context)
{
    EtcdError parse_error(EtcdErrorType::Success);
    auto root = parseJsonObject(body, threadLocalJsonParser(), &parse_error, context);
    if (!root.has_value()) {
        return std::unexpected(std::move(parse_error));
    }

    if (auto error_code = findIntField(root.value(), "code");
        error_code.has_value() && error_code.value() != 0) {
        const std::string message =
            findStringField(root.value(), "message")
                .value_or("etcd server returned error");
        return std::unexpected(EtcdError(
            EtcdErrorType::Server,
            "code=" + std::to_string(error_code.value()) + ", message=" + message));
    }

    return root.value();
}

/**
 * @brief 从 JSON 对象中解析键值对(kvs)数组
 * @param object JSON 对象
 * @param context 错误上下文描述
 * @return 成功返回键值对列表，字段不存在返回空列表，解析失败返回 EtcdError
 */
inline std::expected<std::vector<EtcdKeyValue>, EtcdError> parseKvsFromObject(
    const simdjson::dom::object& object,
    const std::string& context)
{
    auto kvs_field = object["kvs"];
    if (kvs_field.error()) {
        return std::vector<EtcdKeyValue>{};
    }

    auto kvs_array_result = kvs_field.value_unsafe().get_array();
    if (kvs_array_result.error()) {
        return std::unexpected(makeJsonParseError(context + ".kvs as array", kvs_array_result.error()));
    }

    const auto kvs_array = kvs_array_result.value_unsafe();
    std::vector<EtcdKeyValue> kvs;
    kvs.reserve(kvs_array.size());

    auto parseKvObject = [](const simdjson::dom::object& kv_object,
                            const std::string& kv_context) -> std::expected<EtcdKeyValue, EtcdError> {
        const auto encoded_key = findStringField(kv_object, "key");
        if (!encoded_key.has_value()) {
            return std::unexpected(EtcdError(EtcdErrorType::Parse, kv_context + ": missing key in kv item"));
        }

        const auto decoded_key = decodeBase64(encoded_key.value());
        if (!decoded_key.has_value()) {
            return std::unexpected(EtcdError(EtcdErrorType::Parse, kv_context + ": failed to decode base64 key"));
        }

        const auto encoded_value = findStringField(kv_object, "value").value_or("");
        const auto decoded_value = decodeBase64(encoded_value);
        if (!decoded_value.has_value()) {
            return std::unexpected(EtcdError(EtcdErrorType::Parse, kv_context + ": failed to decode base64 value"));
        }

        EtcdKeyValue item;
        item.key = decoded_key.value();
        item.value = decoded_value.value();
        item.create_revision = findIntField(kv_object, "create_revision").value_or(0);
        item.mod_revision = findIntField(kv_object, "mod_revision").value_or(0);
        item.version = findIntField(kv_object, "version").value_or(0);
        item.lease = findIntField(kv_object, "lease").value_or(0);
        return item;
    };

    for (auto kv_element : kvs_array) {
        auto kv_object_result = kv_element.get_object();
        if (kv_object_result.error()) {
            return std::unexpected(makeJsonParseError(context + ".kv item as object", kv_object_result.error()));
        }

        auto parsed_kv = parseKvObject(kv_object_result.value_unsafe(), context);
        if (!parsed_kv.has_value()) {
            return std::unexpected(parsed_kv.error());
        }
        kvs.push_back(std::move(parsed_kv.value()));
    }

    return kvs;
}

/**
 * @brief 解析单个键值对 JSON 对象
 * @details 从 JSON 对象中提取 key、value（Base64 解码）以及
 *          create_revision、mod_revision、version、lease 等元数据。
 * @param kv_object 键值对 JSON 对象
 * @param context 错误上下文描述
 * @return 成功返回 EtcdKeyValue，解析失败返回 EtcdError
 */
inline std::expected<EtcdKeyValue, EtcdError> parseKvObject(
    const simdjson::dom::object& kv_object,
    const std::string& context)
{
    const auto encoded_key = findStringField(kv_object, "key");
    if (!encoded_key.has_value()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, context + ": missing key in kv item"));
    }

    const auto decoded_key = decodeBase64(encoded_key.value());
    if (!decoded_key.has_value()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, context + ": failed to decode base64 key"));
    }

    const auto encoded_value = findStringField(kv_object, "value").value_or("");
    const auto decoded_value = decodeBase64(encoded_value);
    if (!decoded_value.has_value()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, context + ": failed to decode base64 value"));
    }

    EtcdKeyValue item;
    item.key = decoded_key.value();
    item.value = decoded_value.value();
    item.create_revision = findIntField(kv_object, "create_revision").value_or(0);
    item.mod_revision = findIntField(kv_object, "mod_revision").value_or(0);
    item.version = findIntField(kv_object, "version").value_or(0);
    item.lease = findIntField(kv_object, "lease").value_or(0);
    return item;
}

/**
 * @brief 快速判断响应体是否可能包含 etcd 错误字段
 * @details 通过检查 "code"、"message"、"error" 关键字，
 *          用于在解析前快速判断是否需要完整解析。
 * @param body HTTP 响应体
 * @return 可能包含错误字段返回 true，否则返回 false
 */
inline bool maybeContainsEtcdErrorFields(const std::string& body)
{
    return body.find("\"code\"") != std::string::npos ||
        body.find("\"message\"") != std::string::npos ||
        body.find("\"error\"") != std::string::npos;
}

/**
 * @brief 构建 Put 请求的 JSON 请求体
 * @details 将键和值进行 Base64 编码，可选地附加租约 ID，
 *          生成符合 etcd v3 REST API 格式的请求体。
 * @param key 键名
 * @param value 值
 * @param lease_id 可选的租约 ID
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildPutRequestBody(
    std::string_view key,
    std::string_view value,
    std::optional<int64_t> lease_id = std::nullopt)
{
    if (key.empty()) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "key must not be empty"));
    }
    if (lease_id.has_value() && lease_id.value() <= 0) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "lease id must be positive"));
    }

    const std::string encoded_key = encodeBase64(key);
    const std::string encoded_value = encodeBase64(value);
    std::string body;
    body.reserve(40 + encoded_key.size() + encoded_value.size() + (lease_id.has_value() ? 32 : 0));
    body += "{\"key\":\"";
    body += encoded_key;
    body += "\",\"value\":\"";
    body += encoded_value;
    body += "\"";
    if (lease_id.has_value()) {
        body += ",\"lease\":\"";
        body += std::to_string(lease_id.value());
        body += "\"";
    }
    body += "}";
    return body;
}

/**
 * @brief 构建 Range(Get) 请求的 JSON 请求体
 * @details 支持精确匹配和前缀查询，可选返回数量限制。
 * @param key 键名
 * @param prefix 是否为前缀查询
 * @param limit 返回数量限制
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildGetRequestBody(
    std::string_view key,
    bool prefix = false,
    std::optional<int64_t> limit = std::nullopt)
{
    if (key.empty()) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "key must not be empty"));
    }
    if (limit.has_value() && limit.value() <= 0) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "limit must be positive"));
    }

    const std::string encoded_key = encodeBase64(key);
    std::string encoded_range_end;
    if (prefix) {
        encoded_range_end = encodeBase64(makePrefixRangeEnd(std::string(key)));
    }

    std::string body;
    body.reserve(32 + encoded_key.size() + encoded_range_end.size() + (limit.has_value() ? 24 : 0));
    body += "{\"key\":\"";
    body += encoded_key;
    body += "\"";
    if (prefix) {
        body += ",\"range_end\":\"";
        body += encoded_range_end;
        body += "\"";
    }
    if (limit.has_value()) {
        body += ",\"limit\":";
        body += std::to_string(limit.value());
    }
    body += "}";
    return body;
}

/**
 * @brief 构建 DeleteRange 请求的 JSON 请求体
 * @details 支持精确删除和前缀删除。
 * @param key 键名
 * @param prefix 是否为前缀删除
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildDeleteRequestBody(
    std::string_view key,
    bool prefix = false)
{
    if (key.empty()) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "key must not be empty"));
    }

    const std::string encoded_key = encodeBase64(key);
    std::string encoded_range_end;
    if (prefix) {
        encoded_range_end = encodeBase64(makePrefixRangeEnd(std::string(key)));
    }

    std::string body;
    body.reserve(32 + encoded_key.size() + encoded_range_end.size());
    body += "{\"key\":\"";
    body += encoded_key;
    body += "\"";
    if (prefix) {
        body += ",\"range_end\":\"";
        body += encoded_range_end;
        body += "\"";
    }
    body += "}";
    return body;
}

/**
 * @brief 构建 LeaseGrant 请求的 JSON 请求体
 * @param ttl_seconds 租约的存活时间（秒）
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildLeaseGrantRequestBody(int64_t ttl_seconds)
{
    if (ttl_seconds <= 0) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "ttl must be positive"));
    }
    return std::string("{\"TTL\":") + std::to_string(ttl_seconds) + "}";
}

/**
 * @brief 构建 LeaseKeepAlive 请求的 JSON 请求体
 * @param lease_id 需要续期的租约 ID
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildLeaseKeepAliveRequestBody(int64_t lease_id)
{
    if (lease_id <= 0) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "lease id must be positive"));
    }
    return std::string("{\"ID\":\"") + std::to_string(lease_id) + "\"}";
}

/**
 * @brief 构建 Watch 创建请求的 JSON 请求体
 * @details 将键进行 Base64 编码，生成包含 create_request 的请求体。
 * @param key 需要监听的键
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildWatchRequestBody(std::string_view key)
{
    if (key.empty()) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "key must not be empty"));
    }

    return std::string("{\"create_request\":{\"key\":\"") + encodeBase64(key) + "\"}}";
}

/**
 * @brief 构建 Pipeline 事务(Txn) 请求的 JSON 请求体
 * @details 将多个操作（Put/Get/Delete）编码为 etcd v3 事务格式，
 *          所有操作放入 success 分支，compare 为空（无条件执行）。
 * @param operations Pipeline 操作列表
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildTxnBody(std::span<const PipelineOp> operations)
{
    if (operations.empty()) {
        return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "pipeline operations must not be empty"));
    }

    std::string body = "{\"compare\":[],\"success\":[";
    body.reserve(operations.size() * 96 + 32);
    bool first = true;
    for (const auto& op : operations) {
        if (op.key.empty()) {
            return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "pipeline op key must not be empty"));
        }
        if (op.limit.has_value() && op.limit.value() <= 0) {
            return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "pipeline op limit must be positive"));
        }
        if (op.lease_id.has_value() && op.lease_id.value() <= 0) {
            return std::unexpected(EtcdError(EtcdErrorType::InvalidParam, "pipeline op lease id must be positive"));
        }

        if (!first) {
            body += ",";
        }
        first = false;

        switch (op.type) {
        case PipelineOpType::Put: {
            const std::string encoded_key = encodeBase64(op.key);
            const std::string encoded_value = encodeBase64(op.value);
            body += "{\"request_put\":{\"key\":\"";
            body += encoded_key;
            body += "\",\"value\":\"";
            body += encoded_value;
            body += "\"";
            if (op.lease_id.has_value()) {
                body += ",\"lease\":\"" + std::to_string(op.lease_id.value()) + "\"";
            }
            body += "}}";
            break;
        }
        case PipelineOpType::Get: {
            const std::string encoded_key = encodeBase64(op.key);
            body += "{\"request_range\":{\"key\":\"";
            body += encoded_key;
            body += "\"";
            if (op.prefix) {
                body += ",\"range_end\":\"";
                body += encodeBase64(makePrefixRangeEnd(op.key));
                body += "\"";
            }
            if (op.limit.has_value()) {
                body += ",\"limit\":" + std::to_string(op.limit.value());
            }
            body += "}}";
            break;
        }
        case PipelineOpType::Delete: {
            const std::string encoded_key = encodeBase64(op.key);
            body += "{\"request_delete_range\":{\"key\":\"";
            body += encoded_key;
            body += "\"";
            if (op.prefix) {
                body += ",\"range_end\":\"";
                body += encodeBase64(makePrefixRangeEnd(op.key));
                body += "\"";
            }
            body += "}}";
            break;
        }
        }
    }

    body += "],\"failure\":[]}";
    return body;
}

/**
 * @brief 构建 Pipeline 事务(Txn) 请求的 JSON 请求体（vector 重载）
 * @param operations Pipeline 操作列表
 * @return 成功返回 JSON 请求体字符串，参数无效返回 EtcdError
 */
inline std::expected<std::string, EtcdError> buildTxnBody(const std::vector<PipelineOp>& operations)
{
    return buildTxnBody(std::span<const PipelineOp>(operations.data(), operations.size()));
}

/**
 * @brief 解析 Pipeline 事务响应
 * @details 根据 succeeded 字段判断事务是否成功，然后逐条解析
 *          responses 数组中的 Put/Get/Delete 操作结果。
 * @param root 响应 JSON 对象
 * @param operation_types 各操作对应的类型列表
 * @return 成功返回 Pipeline 结果列表，失败返回 EtcdError
 */
inline std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineResponses(
    const simdjson::dom::object& root,
    std::span<const PipelineOpType> operation_types)
{
    auto succeeded_field = root["succeeded"];
    if (!succeeded_field.error()) {
        auto succeeded_result = succeeded_field.value_unsafe().get_bool();
        if (!succeeded_result.error() && !succeeded_result.value_unsafe()) {
            return std::unexpected(EtcdError(EtcdErrorType::Server, "pipeline txn returned succeeded=false"));
        }
    }

    auto responses_field = root["responses"];
    if (responses_field.error()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "pipeline txn response missing responses field"));
    }

    auto responses_array_result = responses_field.value_unsafe().get_array();
    if (responses_array_result.error()) {
        return std::unexpected(makeJsonParseError("parse pipeline responses as array", responses_array_result.error()));
    }

    const auto responses = responses_array_result.value_unsafe();
    if (responses.size() != operation_types.size()) {
        return std::unexpected(EtcdError(
            EtcdErrorType::Parse,
            "pipeline responses size mismatch, expected=" + std::to_string(operation_types.size()) +
                ", actual=" + std::to_string(responses.size())));
    }

    std::vector<PipelineItemResult> pipeline_results;
    pipeline_results.reserve(operation_types.size());

    for (size_t i = 0; i < operation_types.size(); ++i) {
        auto item_object_result = responses.at(i).get_object();
        if (item_object_result.error()) {
            return std::unexpected(makeJsonParseError(
                "parse pipeline response item as object",
                item_object_result.error()));
        }

        const auto item_object = item_object_result.value_unsafe();
        PipelineItemResult item;
        item.type = operation_types[i];

        switch (operation_types[i]) {
        case PipelineOpType::Put: {
            auto put_field = item_object["response_put"];
            if (put_field.error()) {
                return std::unexpected(EtcdError(
                    EtcdErrorType::Parse,
                    "pipeline put response missing response_put"));
            }
            auto put_object_result = put_field.value_unsafe().get_object();
            if (put_object_result.error()) {
                return std::unexpected(makeJsonParseError(
                    "parse pipeline response_put as object",
                    put_object_result.error()));
            }
            item.ok = true;
            break;
        }
        case PipelineOpType::Get: {
            auto range_field = item_object["response_range"];
            if (range_field.error()) {
                return std::unexpected(EtcdError(
                    EtcdErrorType::Parse,
                    "pipeline get response missing response_range"));
            }
            auto range_object_result = range_field.value_unsafe().get_object();
            if (range_object_result.error()) {
                return std::unexpected(makeJsonParseError(
                    "parse pipeline response_range as object",
                    range_object_result.error()));
            }

            auto kvs_result = parseKvsFromObject(
                range_object_result.value_unsafe(),
                "parse pipeline response_range");
            if (!kvs_result.has_value()) {
                return std::unexpected(kvs_result.error());
            }
            item.kvs = std::move(kvs_result.value());
            item.ok = true;
            break;
        }
        case PipelineOpType::Delete: {
            auto del_field = item_object["response_delete_range"];
            if (del_field.error()) {
                return std::unexpected(EtcdError(
                    EtcdErrorType::Parse,
                    "pipeline delete response missing response_delete_range"));
            }
            auto del_object_result = del_field.value_unsafe().get_object();
            if (del_object_result.error()) {
                return std::unexpected(makeJsonParseError(
                    "parse pipeline response_delete_range as object",
                    del_object_result.error()));
            }
            item.deleted_count = findIntField(del_object_result.value_unsafe(), "deleted").value_or(0);
            item.ok = true;
            break;
        }
        }

        pipeline_results.push_back(std::move(item));
    }

    return pipeline_results;
}

/**
 * @brief 解析 Pipeline 事务响应（PipelineOp 重载）
 * @details 从操作列表中提取类型信息后委托给类型版本的重载。
 * @param root 响应 JSON 对象
 * @param operations Pipeline 操作列表
 * @return 成功返回 Pipeline 结果列表，失败返回 EtcdError
 */
inline std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineResponses(
    const simdjson::dom::object& root,
    std::span<const PipelineOp> operations)
{
    std::vector<PipelineOpType> operation_types;
    operation_types.reserve(operations.size());
    for (const auto& op : operations) {
        operation_types.push_back(op.type);
    }
    return parsePipelineResponses(root, std::span<const PipelineOpType>(operation_types.data(), operation_types.size()));
}

/**
 * @brief 解析 Put 操作响应
 * @details 检查响应体是否包含错误字段，若有则完整解析并返回错误。
 * @param body HTTP 响应体
 * @return 成功返回 void，失败返回 EtcdError
 */
inline std::expected<void, EtcdError> parsePutResponse(const std::string& body)
{
    if (!maybeContainsEtcdErrorFields(body)) {
        return {};
    }

    auto root = parseEtcdSuccessObject(body, "parse put response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    return {};
}

/**
 * @brief 解析 Range(Get) 操作响应
 * @details 解析响应体中的 kvs 数组，返回键值对列表。
 * @param body HTTP 响应体
 * @return 成功返回键值对列表，失败返回 EtcdError
 */
inline std::expected<std::vector<EtcdKeyValue>, EtcdError> parseGetResponseKvs(const std::string& body)
{
    auto root = parseEtcdSuccessObject(body, "parse get response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    return parseKvsFromObject(root.value(), "parse get response");
}

/**
 * @brief 解析 DeleteRange 操作响应
 * @details 从响应体中提取 deleted 字段，返回删除的键数量。
 * @param body HTTP 响应体
 * @return 成功返回删除数量，失败返回 EtcdError
 */
inline std::expected<int64_t, EtcdError> parseDeleteResponseDeletedCount(const std::string& body)
{
    auto root = parseEtcdSuccessObject(body, "parse delete response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    return findIntField(root.value(), "deleted").value_or(0);
}

/**
 * @brief 解析 LeaseGrant 操作响应
 * @details 从响应体中提取 ID 字段，返回分配的租约 ID。
 * @param body HTTP 响应体
 * @return 成功返回租约 ID，失败返回 EtcdError
 */
inline std::expected<int64_t, EtcdError> parseLeaseGrantResponseId(const std::string& body)
{
    auto root = parseEtcdSuccessObject(body, "parse lease grant response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }

    const auto lease_id = findIntField(root.value(), "ID");
    if (!lease_id.has_value()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "lease grant response missing ID"));
    }
    return lease_id.value();
}

/**
 * @brief 解析 LeaseKeepAlive 操作响应
 * @details 从响应体中提取 ID 字段并与期望的租约 ID 进行比对。
 * @param body HTTP 响应体
 * @param expected_lease_id 期望续期的租约 ID
 * @return 成功返回租约 ID，ID 不匹配或解析失败返回 EtcdError
 */
inline std::expected<int64_t, EtcdError> parseLeaseKeepAliveResponseId(
    const std::string& body,
    int64_t expected_lease_id)
{
    auto root = parseEtcdSuccessObject(body, "parse lease keepalive response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }

    const auto response_id = findIntField(root.value(), "ID");
    if (response_id.has_value() && response_id.value() != expected_lease_id) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "lease keepalive response id mismatch"));
    }
    return expected_lease_id;
}

/**
 * @brief 解析 Pipeline 事务(Txn) 响应（操作类型版本）
 * @details 先解析顶层响应体，再委托给 parsePipelineResponses 进行逐条解析。
 * @param body HTTP 响应体
 * @param operation_types 各操作对应的类型列表
 * @return 成功返回 Pipeline 结果列表，失败返回 EtcdError
 */
inline std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineTxnResponse(
    const std::string& body,
    std::span<const PipelineOpType> operation_types)
{
    auto root = parseEtcdSuccessObject(body, "parse pipeline txn response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    return parsePipelineResponses(root.value(), operation_types);
}

/**
 * @brief 解析 Pipeline 事务(Txn) 响应（PipelineOp 版本）
 * @details 先解析顶层响应体，再委托给 parsePipelineResponses 进行逐条解析。
 * @param body HTTP 响应体
 * @param operations Pipeline 操作列表
 * @return 成功返回 Pipeline 结果列表，失败返回 EtcdError
 */
inline std::expected<std::vector<PipelineItemResult>, EtcdError> parsePipelineTxnResponse(
    const std::string& body,
    std::span<const PipelineOp> operations)
{
    auto root = parseEtcdSuccessObject(body, "parse pipeline txn response");
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    return parsePipelineResponses(root.value(), operations);
}

/**
 * @brief 解析 Watch 响应
 * @details 解析 watch 事件流的单个 JSON 响应，提取 watch_id、created、canceled、
 *          compact_revision 等元信息，以及事件列表中每个事件的类型(PUT/DELETE)
 *          和键值对（包含可选的 prev_kv）。
 * @param body Watch 响应体字符串
 * @return 成功返回 EtcdWatchResponse，解析失败返回 EtcdError
 */
inline std::expected<EtcdWatchResponse, EtcdError> parseWatchResponse(const std::string& body)
{
    EtcdError parse_error(EtcdErrorType::Success);
    auto root = parseJsonObject(body, threadLocalJsonParser(), &parse_error, "parse watch response");
    if (!root.has_value()) {
        return std::unexpected(std::move(parse_error));
    }

    auto result_field = root.value()["result"];
    if (result_field.error()) {
        return std::unexpected(EtcdError(EtcdErrorType::Parse, "watch response missing result"));
    }

    auto result_object_result = result_field.value_unsafe().get_object();
    if (result_object_result.error()) {
        return std::unexpected(makeJsonParseError("parse watch result as object", result_object_result.error()));
    }

    const auto result_object = result_object_result.value_unsafe();

    EtcdWatchResponse response;
    response.watch_id = findIntField(result_object, "watch_id").value_or(0);
    response.compact_revision = findIntField(result_object, "compact_revision").value_or(0);

    if (auto created_field = result_object["created"]; !created_field.error()) {
        auto created_result = created_field.value_unsafe().get_bool();
        if (!created_result.error()) {
            response.created = created_result.value_unsafe();
        }
    }

    if (auto canceled_field = result_object["canceled"]; !canceled_field.error()) {
        auto canceled_result = canceled_field.value_unsafe().get_bool();
        if (!canceled_result.error()) {
            response.canceled = canceled_result.value_unsafe();
        }
    }

    auto events_field = result_object["events"];
    if (events_field.error()) {
        return response;
    }

    auto events_array_result = events_field.value_unsafe().get_array();
    if (events_array_result.error()) {
        return std::unexpected(makeJsonParseError("parse watch events as array", events_array_result.error()));
    }

    for (auto event_element : events_array_result.value_unsafe()) {
        auto event_object_result = event_element.get_object();
        if (event_object_result.error()) {
            return std::unexpected(makeJsonParseError("parse watch event as object", event_object_result.error()));
        }

        const auto event_object = event_object_result.value_unsafe();
        EtcdWatchEvent event;

        if (auto type_string = findStringField(event_object, "type"); type_string.has_value()) {
            if (type_string.value() == "PUT") {
                event.type = EtcdWatchEventType::Put;
            } else if (type_string.value() == "DELETE") {
                event.type = EtcdWatchEventType::Delete;
            } else {
                event.type = EtcdWatchEventType::Unknown;
            }
        }

        auto kv_field = event_object["kv"];
        if (!kv_field.error()) {
            auto kv_object_result = kv_field.value_unsafe().get_object();
            if (kv_object_result.error()) {
                return std::unexpected(makeJsonParseError("parse watch kv as object", kv_object_result.error()));
            }
            auto kv_result = parseKvObject(kv_object_result.value_unsafe(), "parse watch kv");
            if (!kv_result.has_value()) {
                return std::unexpected(kv_result.error());
            }
            event.kv = std::move(kv_result.value());
            if (event.type == EtcdWatchEventType::Unknown) {
                event.type = event.kv.value.empty()
                    ? EtcdWatchEventType::Delete
                    : EtcdWatchEventType::Put;
            }
        }

        auto prev_kv_field = event_object["prev_kv"];
        if (!prev_kv_field.error()) {
            auto prev_kv_object_result = prev_kv_field.value_unsafe().get_object();
            if (prev_kv_object_result.error()) {
                return std::unexpected(makeJsonParseError("parse watch prev_kv as object", prev_kv_object_result.error()));
            }
            auto prev_kv_result = parseKvObject(prev_kv_object_result.value_unsafe(), "parse watch prev_kv");
            if (!prev_kv_result.has_value()) {
                return std::unexpected(prev_kv_result.error());
            }
            event.prev_kv = std::move(prev_kv_result.value());
        }

        response.events.push_back(std::move(event));
    }

    return response;
}

} // namespace galay::etcd::internal

#endif // GALAY_ETCD_INTERNAL_H
