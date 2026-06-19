/**
 * @file otlp_http_exporter.cc
 * @brief OTLP/HTTP JSON Span 导出器实现
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 实现 Span 到 OTLP JSON 的序列化、请求构建和 HTTP 传输。
 * 可选启用基于 galay-http 协程的内置传输，或由用户提供自定义传输函数。
 */

#include "galay-tracing/kernel/otlp_http_exporter.h"

#include "galay-tracing/common/tracing_log.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>

#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
#include "galay-http/client/http_client.h"
#include "galay-kernel/core/runtime.h"

#include <map>
#endif

namespace galay::tracing {

namespace {

[[nodiscard]] bool asciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto lower_lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        const auto lower_rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
        if (lower_lhs != lower_rhs) {
            return false;
        }
    }
    return true;
}

void appendJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
        case '\\':
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (ch < 0x20) {
                constexpr char kHex[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(kHex[ch >> 4]);
                out.push_back(kHex[ch & 0x0f]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] bool hasHeader(std::span<const OtlpHttpHeader> headers, std::string_view name) {
    return std::ranges::any_of(headers, [name](const OtlpHttpHeader& header) {
        return asciiEqualsIgnoreCase(header.name, name);
    });
}

template <typename Id>
void appendHexId(std::string& out, const Id& id) {
    const auto hex = id.toHexArray();
    out.append(hex.data(), hex.size());
}

[[nodiscard]] std::string_view otlpSpanKindName(SpanKind kind) noexcept {
    switch (kind) {
    case SpanKind::kServer:
        return "SPAN_KIND_SERVER";
    case SpanKind::kClient:
        return "SPAN_KIND_CLIENT";
    case SpanKind::kProducer:
        return "SPAN_KIND_PRODUCER";
    case SpanKind::kConsumer:
        return "SPAN_KIND_CONSUMER";
    case SpanKind::kInternal:
    default:
        return "SPAN_KIND_INTERNAL";
    }
}

[[nodiscard]] std::string_view otlpStatusCodeName(SpanStatusCode code) noexcept {
    switch (code) {
    case SpanStatusCode::kOk:
        return "STATUS_CODE_OK";
    case SpanStatusCode::kError:
        return "STATUS_CODE_ERROR";
    case SpanStatusCode::kUnset:
    default:
        return "STATUS_CODE_UNSET";
    }
}

template <typename Number>
void appendNumber(std::string& out, Number value) {
    std::array<char, 32> buffer{};
    auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) {
        out.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    }
}

void appendAttributeValue(std::string& out, const SpanAttributeValue& value) {
    switch (value.type()) {
    case SpanAttributeType::kInt64:
        out.append("{\"intValue\":\"");
        appendNumber(out, value.asInt64());
        out.append("\"}");
        break;
    case SpanAttributeType::kUInt64:
        out.append("{\"intValue\":\"");
        appendNumber(out, value.asUInt64());
        out.append("\"}");
        break;
    case SpanAttributeType::kDouble:
        out.append("{\"doubleValue\":");
        appendNumber(out, value.asDouble());
        out.push_back('}');
        break;
    case SpanAttributeType::kBool:
        out.append(value.asBool() ? "{\"boolValue\":true}" : "{\"boolValue\":false}");
        break;
    case SpanAttributeType::kString:
        out.append("{\"stringValue\":");
        appendJsonString(out, value.asString());
        out.push_back('}');
        break;
    }
}

void appendAttributeArray(std::string& out, std::span<const SpanAttribute> attributes) {
    out.push_back('[');
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"key\":");
        appendJsonString(out, attributes[i].name);
        out.append(",\"value\":");
        appendAttributeValue(out, attributes[i].value);
        out.push_back('}');
    }
    out.push_back(']');
}

void appendAttributes(std::string& out, std::span<const SpanAttribute> attributes) {
    out.append(",\"attributes\":");
    appendAttributeArray(out, attributes);
}

void appendTimeUnixNano(std::string& out, Span::Clock::time_point timestamp) {
    out.append("\"timeUnixNano\":\"");
    appendNumber(out, std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp.time_since_epoch()).count());
    out.push_back('"');
}

void appendEvents(std::string& out, std::span<const SpanEvent> events) {
    if (events.empty()) {
        return;
    }
    out.append(",\"events\":[");
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.push_back('{');
        bool has_field = false;
        if (events[i].timestamp != Span::Clock::time_point{}) {
            appendTimeUnixNano(out, events[i].timestamp);
            has_field = true;
        }
        if (has_field) {
            out.push_back(',');
        }
        out.append("\"name\":");
        appendJsonString(out, events[i].name);
        if (!events[i].attributes.empty()) {
            appendAttributes(out, events[i].attributes);
        }
        out.push_back('}');
    }
    out.push_back(']');
}

void appendLinks(std::string& out, std::span<const SpanLink> links) {
    if (links.empty()) {
        return;
    }
    out.append(",\"links\":[");
    for (std::size_t i = 0; i < links.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append("{\"traceId\":\"");
        appendHexId(out, links[i].context.traceId());
        out.append("\",\"spanId\":\"");
        appendHexId(out, links[i].context.spanId());
        out.push_back('"');
        if (!links[i].tracestate.empty()) {
            out.append(",\"traceState\":");
            appendJsonString(out, links[i].tracestate);
        }
        if (!links[i].attributes.empty()) {
            appendAttributes(out, links[i].attributes);
        }
        out.push_back('}');
    }
    out.push_back(']');
}

void appendResource(std::string& out, std::span<const SpanAttribute> attributes) {
    if (attributes.empty()) {
        return;
    }
    out.append("\"resource\":{\"attributes\":");
    appendAttributeArray(out, attributes);
    out.append("},");
}

void appendScope(std::string& out, const InstrumentationScopeConfig& scope) {
    out.append("\"scope\":{\"name\":");
    appendJsonString(out, scope.name);
    if (!scope.version.empty()) {
        out.append(",\"version\":");
        appendJsonString(out, scope.version);
    }
    out.push_back('}');
}

void appendStatus(std::string& out, const SpanStatus& status) {
    if (status.code == SpanStatusCode::kUnset && status.message.empty()) {
        return;
    }
    out.append(",\"status\":{\"code\":\"");
    out.append(otlpStatusCodeName(status.code));
    out.push_back('"');
    if (!status.message.empty()) {
        out.append(",\"message\":");
        appendJsonString(out, status.message);
    }
    out.push_back('}');
}

void addAttributeEstimate(std::size_t& size, std::span<const SpanAttribute> attributes) noexcept {
    for (const auto& attribute : attributes) {
        size += attribute.name.size() + 48;
        if (attribute.value.type() == SpanAttributeType::kString) {
            size += attribute.value.asString().size();
        }
    }
}

[[nodiscard]] std::size_t estimateOtlpJsonBodySize(
    std::span<const Span> spans,
    const OtlpHttpExporterConfig& config) noexcept {
    std::size_t size = 128 + config.scope.name.size() + config.scope.version.size();
    addAttributeEstimate(size, config.resource_attributes);
    for (const auto& span : spans) {
        size += 160 + span.name().size();
        const auto& context = span.spanContext();
        if (context.parentSpanId().has_value()) {
            size += SpanId::kHexLength + 18;
        }
        if (!span.tracestate().empty()) {
            size += span.tracestate().size() + 16;
        }
        if (span.status().code != SpanStatusCode::kUnset || !span.status().message.empty()) {
            size += span.status().message.size() + 64;
        }
        addAttributeEstimate(size, span.attributes());
        for (const auto& event : span.events()) {
            size += event.name.size() + 48;
            addAttributeEstimate(size, event.attributes);
        }
        for (const auto& link : span.links()) {
            size += 128 + link.tracestate.size();
            addAttributeEstimate(size, link.attributes);
        }
    }
    return size;
}

[[nodiscard]] std::string buildOtlpJsonBody(std::span<const Span> spans, const OtlpHttpExporterConfig& config) {
    std::string body;
    body.reserve(estimateOtlpJsonBodySize(spans, config));
    body.append("{\"resourceSpans\":[{");
    appendResource(body, config.resource_attributes);
    body.append("\"scopeSpans\":[{");
    appendScope(body, config.scope);
    body.append(",\"spans\":[");
    for (std::size_t i = 0; i < spans.size(); ++i) {
        const auto& span = spans[i];
        const auto& context = span.spanContext();
        if (i != 0) {
            body.push_back(',');
        }
        body.append("{\"traceId\":\"");
        appendHexId(body, context.traceId());
        body.append("\",\"spanId\":\"");
        appendHexId(body, context.spanId());
        body.append("\",\"name\":");
        appendJsonString(body, span.name());
        body.append(",\"kind\":\"");
        body.append(otlpSpanKindName(span.kind()));
        body.push_back('"');
        if (context.parentSpanId().has_value()) {
            body.append(",\"parentSpanId\":\"");
            appendHexId(body, *context.parentSpanId());
            body.push_back('"');
        }
        if (!span.tracestate().empty()) {
            body.append(",\"traceState\":");
            appendJsonString(body, span.tracestate());
        }
        if (!span.attributes().empty()) {
            appendAttributes(body, span.attributes());
        }
        appendEvents(body, span.events());
        appendLinks(body, span.links());
        appendStatus(body, span.status());
        body.push_back('}');
    }
    body.append("]}]}]}");
    return body;
}

[[nodiscard]] std::vector<OtlpHttpHeader> makeHeaders(const OtlpHttpExporterConfig& config) {
    std::vector<OtlpHttpHeader> headers;
    headers.reserve(config.headers.size() + 1);
    if (!hasHeader(config.headers, "content-type")) {
        headers.push_back({"content-type", "application/json"});
    }
    headers.insert(headers.end(), config.headers.begin(), config.headers.end());
    return headers;
}

[[nodiscard]] OtlpHttpTransport makeUnavailableTransport() {
    return [](OtlpHttpRequest) {
        return OtlpHttpResponse{
            .status_code = 0,
            .error = "GALAY_TRACING_ENABLE_OTLP_HTTP is disabled and no custom OTLP transport was supplied",
        };
    };
}

[[nodiscard]] OtlpHttpTransport makeDefaultTransport() {
#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
    return makeGalayHttpOtlpTransport();
#else
    return makeUnavailableTransport();
#endif
}

#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
[[nodiscard]] bool hasMappedHeader(const std::map<std::string, std::string>& headers, std::string_view name) {
    return std::ranges::any_of(headers, [name](const auto& entry) {
        return asciiEqualsIgnoreCase(entry.first, name);
    });
}

[[nodiscard]] std::string endpointHostHeader(const galay::http::HttpUrl& url) {
    const bool default_port = (!url.is_secure && url.port == 80) || (url.is_secure && url.port == 443);
    if (default_port) {
        return url.host;
    }
    return url.host + ":" + std::to_string(url.port);
}

galay::kernel::Task<OtlpHttpResponse> sendWithGalayHttp(OtlpHttpRequest request) {
    try {
        const std::string endpoint(request.endpoint);
        auto parsed = galay::http::HttpUrl::parse(endpoint);
        if (!parsed.has_value()) {
            co_return OtlpHttpResponse{.status_code = 0, .error = "invalid OTLP HTTP endpoint"};
        }
        if (parsed->is_secure) {
            co_return OtlpHttpResponse{.status_code = 0, .error = "https OTLP endpoints require a TLS transport"};
        }

        std::string content_type = "application/json";
        std::map<std::string, std::string> headers;
        for (const auto& header : request.headers) {
            if (asciiEqualsIgnoreCase(header.name, "content-type")) {
                content_type = header.value;
                continue;
            }
            headers[header.name] = header.value;
        }
        if (!hasMappedHeader(headers, "host")) {
            headers["Host"] = endpointHostHeader(*parsed);
        }
        if (!hasMappedHeader(headers, "accept")) {
            headers["Accept"] = "application/json";
        }
        if (!hasMappedHeader(headers, "user-agent")) {
            headers["User-Agent"] = "galay-tracing";
        }
        if (!hasMappedHeader(headers, "connection")) {
            headers["Connection"] = "close";
        }

        auto client = galay::http::HttpClientBuilder().build();
        auto connect_result = co_await client.connect(endpoint);
        if (!connect_result) {
            co_return OtlpHttpResponse{.status_code = 0, .error = std::string(connect_result.error().message())};
        }

        auto session_result = client.getSession();
        if (!session_result) {
            static_cast<void>(co_await client.close());
            co_return OtlpHttpResponse{.status_code = 0, .error = std::string(session_result.error().message())};
        }

        auto result = co_await session_result.value()->post(parsed->path, std::move(request.body), content_type, headers)
            .timeout(request.timeout);
        if (!result) {
            static_cast<void>(co_await client.close());
            co_return OtlpHttpResponse{.status_code = 0, .error = std::string(result.error().message())};
        }
        if (!result.value().has_value()) {
            static_cast<void>(co_await client.close());
            co_return OtlpHttpResponse{.status_code = 0, .error = "incomplete OTLP HTTP response"};
        }

        auto response = std::move(result.value().value());
        auto body = response.getBodyStr();
        const int status_code = static_cast<int>(response.header().code());
        static_cast<void>(co_await client.close());
        co_return OtlpHttpResponse{.status_code = status_code, .body = std::move(body)};
    } catch (const std::exception& error) {
        co_return OtlpHttpResponse{.status_code = 0, .error = error.what()};
    } catch (...) {
        co_return OtlpHttpResponse{.status_code = 0, .error = "unknown OTLP HTTP transport error"};
    }
}

class GalayHttpTransportState {
public:
    explicit GalayHttpTransportState(GalayHttpOtlpTransportConfig config)
        : m_config(std::move(config)) {
        m_config.io_scheduler_count = std::max<std::size_t>(m_config.io_scheduler_count, 1);
    }

    OtlpHttpResponse send(OtlpHttpRequest request) {
        if (m_config.reject_on_runtime_thread && galay::kernel::RuntimeHandle::tryCurrent().has_value()) {
            return OtlpHttpResponse{
                .status_code = 0,
                .error = "synchronous OTLP export is not allowed on a galay scheduler thread",
            };
        }

        std::lock_guard lock(m_mutex);
        ensureRuntime();
        auto join = m_runtime->spawn(sendWithGalayHttp(std::move(request)));
        if (!join) {
            return OtlpHttpResponse{.status_code = 0, .error = "failed to spawn OTLP HTTP transport task"};
        }
        auto result = join->join();
        if (!result) {
            return OtlpHttpResponse{.status_code = 0, .error = "failed to join OTLP HTTP transport task"};
        }
        return std::move(result.value());
    }

private:
    void ensureRuntime() {
        if (m_runtime) {
            return;
        }

        auto runtime_config = galay::kernel::RuntimeBuilder()
            .ioSchedulerCount(m_config.io_scheduler_count)
            .computeSchedulerCount(0)
            .buildConfig();
        m_runtime = std::make_unique<galay::kernel::Runtime>(runtime_config);
        m_runtime->start();
    }

    GalayHttpOtlpTransportConfig m_config;
    std::mutex m_mutex;
    std::unique_ptr<galay::kernel::Runtime> m_runtime;
};
#endif

} // namespace

#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)
OtlpHttpTransport makeGalayHttpOtlpTransport(GalayHttpOtlpTransportConfig config) {
    auto state = std::make_shared<GalayHttpTransportState>(std::move(config));
    return [state = std::move(state)](OtlpHttpRequest request) {
        return state->send(std::move(request));
    };
}
#endif

OtlpHttpExporter::OtlpHttpExporter(OtlpHttpExporterConfig config)
    : OtlpHttpExporter(std::move(config), makeDefaultTransport()) {
}

OtlpHttpExporter::OtlpHttpExporter(OtlpHttpExporterConfig config, OtlpHttpTransport transport)
    : m_config(std::move(config)),
      m_headers(makeHeaders(m_config)),
      m_transport(std::move(transport)) {
    if (!m_transport) {
        m_transport = makeUnavailableTransport();
    }
}

ExportResult OtlpHttpExporter::exportSpans(std::span<const Span> spans) {
    if (spans.empty()) {
        return ExportResult::kSuccess;
    }

    TRACING_LOG_DEBUG("[otlp_http_exporter]", "export spans span_count={} endpoint={}",
                      spans.size(),
                      m_config.endpoint);

    OtlpHttpResponse response;
    try {
        response = m_transport(OtlpHttpRequest{
            .method = "POST",
            .endpoint = m_config.endpoint,
            .timeout = m_config.timeout,
            .headers = m_headers,
            .body = buildOtlpJsonBody(spans, m_config),
        });
    } catch (...) {
        TRACING_LOG_ERROR("[otlp_http_exporter]", "transport threw span_count={}", spans.size());
        return ExportResult::kFailure;
    }

    if (response.status_code >= 200 && response.status_code < 300) {
        TRACING_LOG_DEBUG("[otlp_http_exporter]", "export succeeded status={} body_size={}",
                          response.status_code,
                          response.body.size());
        return ExportResult::kSuccess;
    }

    TRACING_LOG_WARN("[otlp_http_exporter]", "export failed status={} error={}",
                     response.status_code,
                     response.error);
    return ExportResult::kFailure;
}

bool OtlpHttpExporter::forceFlush(std::chrono::milliseconds) {
    return true;
}

bool OtlpHttpExporter::shutdown(std::chrono::milliseconds) {
    return true;
}

} // namespace galay::tracing
