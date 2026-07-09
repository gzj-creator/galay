#include <galay/cpp/galay-tracing/kernel/file_span_exporter.h>
#include <galay/cpp/galay-tracing/kernel/otlp_http_exporter.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

galay::tracing::TraceContext makeContext(std::string_view span_id = "00f067aa0ba902b7") {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::fromHex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::fromHex(span_id),
        0x01,
        "vendor=value");
}

galay::tracing::SpanContext makeLinkedContext() {
    return galay::tracing::SpanContext(makeContext("1111111111111111"));
}

void spanStoresBoundedEventsWithAttributes() {
    galay::tracing::Span span("events", makeContext());

    std::vector<galay::tracing::SpanAttribute> attributes;
    for (std::size_t i = 0; i < galay::tracing::Span::kMaxEventAttributes; ++i) {
        attributes.push_back(galay::tracing::spanAttribute("event.attr", static_cast<int>(i)));
    }
    attributes.push_back(galay::tracing::spanAttribute("overflow", true));

    assert(span.addEvent("cache.miss", attributes));
    assert(span.events().size() == 1);
    assert(span.events()[0].name == "cache.miss");
    assert(span.events()[0].attributes.size() == galay::tracing::Span::kMaxEventAttributes);
    assert(span.events()[0].attributes.back().name == "event.attr");

    for (std::size_t i = 1; i < galay::tracing::Span::kMaxEvents; ++i) {
        assert(span.addEvent("bounded"));
    }
    assert(!span.addEvent("dropped"));
    assert(span.events().size() == galay::tracing::Span::kMaxEvents);

    std::span<const galay::tracing::SpanEvent> readonly_events = span.events();
    assert(readonly_events.front().name == "cache.miss");
}

void spanStoresBoundedLinksWithAttributes() {
    galay::tracing::Span span("links", makeContext());

    std::vector<galay::tracing::SpanAttribute> attributes;
    for (std::size_t i = 0; i < galay::tracing::Span::kMaxLinkAttributes; ++i) {
        attributes.push_back(galay::tracing::spanAttribute("link.attr", static_cast<int>(i)));
    }
    attributes.push_back(galay::tracing::spanAttribute("overflow", false));

    assert(span.addLink(makeLinkedContext(), "linkstate=1", attributes));
    assert(span.links().size() == 1);
    assert(span.links()[0].context.spanId().toHex() == "1111111111111111");
    assert(span.links()[0].tracestate == "linkstate=1");
    assert(span.links()[0].attributes.size() == galay::tracing::Span::kMaxLinkAttributes);
    assert(span.links()[0].attributes.back().name == "link.attr");

    for (std::size_t i = 1; i < galay::tracing::Span::kMaxLinks; ++i) {
        assert(span.addLink(makeLinkedContext()));
    }
    assert(!span.addLink(makeLinkedContext()));
    assert(span.links().size() == galay::tracing::Span::kMaxLinks);

    std::span<const galay::tracing::SpanLink> readonly_links = span.links();
    assert(readonly_links.front().context.traceId().toHex() == "4bf92f3577b34da6a3ce929d0e0e4736");
}

void otlpJsonExporterEncodesEventsAndLinks() {
    bool captured = false;
    auto transport = [&](galay::tracing::OtlpHttpRequest request) {
        captured = true;
        assert(request.body.find("\"events\"") != std::string::npos);
        assert(request.body.find("\"name\":\"cache.miss\"") != std::string::npos);
        assert(request.body.find("\"key\":\"cache.key\",\"value\":{\"stringValue\":\"user:42\"}") != std::string::npos);
        assert(request.body.find("\"links\"") != std::string::npos);
        assert(request.body.find("\"spanId\":\"1111111111111111\"") != std::string::npos);
        assert(request.body.find("\"traceState\":\"linkstate=1\"") != std::string::npos);
        assert(request.body.find("\"key\":\"link.type\",\"value\":{\"stringValue\":\"batch\"}") != std::string::npos);
        return galay::tracing::OtlpHttpResponse{.status_code = 200};
    };

    galay::tracing::Span span("export", makeContext());
    assert(span.addEvent("cache.miss", {galay::tracing::spanAttribute("cache.key", "user:42")}));
    assert(span.addLink(
        makeLinkedContext(),
        "linkstate=1",
        {galay::tracing::spanAttribute("link.type", "batch")}));
    span.end();

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector<galay::tracing::Span> spans;
    spans.push_back(std::move(span));
    assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(captured);
}

void fileExporterEncodesEventsAndLinks() {
    const auto path = std::filesystem::temp_directory_path() / "galay-tracing-t12-events-links.jsonl";
    std::filesystem::remove(path);

    galay::tracing::Span span("file", makeContext());
    assert(span.addEvent("file.event", {galay::tracing::spanAttribute("file.attr", 7)}));
    assert(span.addLink(makeLinkedContext(), {}, {galay::tracing::spanAttribute("link.attr", true)}));
    span.end();

    {
        galay::tracing::FileSpanExporter exporter(path);
        std::vector<galay::tracing::Span> spans;
        spans.push_back(std::move(span));
        assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
        assert(exporter.forceFlush(std::chrono::milliseconds(0)));
    }

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    assert(line.find("\"events\"") != std::string::npos);
    assert(line.find("\"name\":\"file.event\"") != std::string::npos);
    assert(line.find("\"links\"") != std::string::npos);
    assert(line.find("\"span_id\":\"1111111111111111\"") != std::string::npos);

    std::filesystem::remove(path);
}

void fileExporterEscapesJsonlControlCharacters() {
    const auto path = std::filesystem::temp_directory_path() / "galay-tracing-t12-control-chars.jsonl";
    std::filesystem::remove(path);

    galay::tracing::Span span("line\nname", makeContext());
    assert(span.addEvent("tab\tand\x01" "control", {galay::tracing::spanAttribute("attr\nkey", "value\r\nnext")}));
    span.end();

    {
        galay::tracing::FileSpanExporter exporter(path);
        std::vector<galay::tracing::Span> spans;
        spans.push_back(std::move(span));
        assert(exporter.exportSpans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
        assert(exporter.forceFlush(std::chrono::milliseconds(0)));
    }

    std::ifstream in(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    assert(bytes.find("\"name\":\"line\\nname\"") != std::string::npos);
    assert(bytes.find("\"name\":\"tab\\tand\\u0001control\"") != std::string::npos);
    assert(bytes.find("\"key\":\"attr\\nkey\"") != std::string::npos);
    assert(bytes.find("\"value\":\"value\\r\\nnext\"") != std::string::npos);
    assert(bytes.find("line\nname") == std::string::npos);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    spanStoresBoundedEventsWithAttributes();
    spanStoresBoundedLinksWithAttributes();
    otlpJsonExporterEncodesEventsAndLinks();
    fileExporterEncodesEventsAndLinks();
    fileExporterEscapesJsonlControlCharacters();
}
