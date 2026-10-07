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

galay::tracing::TraceContext make_context(std::string_view span_id = "00f067aa0ba902b7") {
    return galay::tracing::TraceContext(
        galay::tracing::TraceId::from_hex("4bf92f3577b34da6a3ce929d0e0e4736"),
        galay::tracing::SpanId::from_hex(span_id),
        0x01,
        "vendor=value");
}

galay::tracing::SpanContext make_linked_context() {
    return galay::tracing::SpanContext(make_context("1111111111111111"));
}

void span_stores_bounded_events_with_attributes() {
    galay::tracing::Span span("events", make_context());

    std::vector<galay::tracing::SpanAttribute> attributes;
    for (std::size_t i = 0; i < galay::tracing::Span::kMaxEventAttributes; ++i) {
        attributes.push_back(galay::tracing::span_attribute("event.attr", static_cast<int>(i)));
    }
    attributes.push_back(galay::tracing::span_attribute("overflow", true));

    assert(span.add_event("cache.miss", attributes));
    assert(span.events().size() == 1);
    assert(span.events()[0].name == "cache.miss");
    assert(span.events()[0].attributes.size() == galay::tracing::Span::kMaxEventAttributes);
    assert(span.events()[0].attributes.back().name == "event.attr");

    for (std::size_t i = 1; i < galay::tracing::Span::kMaxEvents; ++i) {
        assert(span.add_event("bounded"));
    }
    assert(!span.add_event("dropped"));
    assert(span.events().size() == galay::tracing::Span::kMaxEvents);

    std::span<const galay::tracing::SpanEvent> readonly_events = span.events();
    assert(readonly_events.front().name == "cache.miss");
}

void span_stores_bounded_links_with_attributes() {
    galay::tracing::Span span("links", make_context());

    std::vector<galay::tracing::SpanAttribute> attributes;
    for (std::size_t i = 0; i < galay::tracing::Span::kMaxLinkAttributes; ++i) {
        attributes.push_back(galay::tracing::span_attribute("link.attr", static_cast<int>(i)));
    }
    attributes.push_back(galay::tracing::span_attribute("overflow", false));

    assert(span.add_link(make_linked_context(), "linkstate=1", attributes));
    assert(span.links().size() == 1);
    assert(span.links()[0].context.span_id().to_hex() == "1111111111111111");
    assert(span.links()[0].tracestate == "linkstate=1");
    assert(span.links()[0].attributes.size() == galay::tracing::Span::kMaxLinkAttributes);
    assert(span.links()[0].attributes.back().name == "link.attr");

    for (std::size_t i = 1; i < galay::tracing::Span::kMaxLinks; ++i) {
        assert(span.add_link(make_linked_context()));
    }
    assert(!span.add_link(make_linked_context()));
    assert(span.links().size() == galay::tracing::Span::kMaxLinks);

    std::span<const galay::tracing::SpanLink> readonly_links = span.links();
    assert(readonly_links.front().context.trace_id().to_hex() == "4bf92f3577b34da6a3ce929d0e0e4736");
}

void otlp_json_exporter_encodes_events_and_links() {
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

    galay::tracing::Span span("export", make_context());
    assert(span.add_event("cache.miss", {galay::tracing::span_attribute("cache.key", "user:42")}));
    assert(span.add_link(
        make_linked_context(),
        "linkstate=1",
        {galay::tracing::span_attribute("link.type", "batch")}));
    span.end();

    galay::tracing::OtlpHttpExporter exporter({}, transport);
    std::vector<galay::tracing::Span> spans;
    spans.push_back(std::move(span));
    assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
    assert(captured);
}

void file_exporter_encodes_events_and_links() {
    const auto path = std::filesystem::temp_directory_path() / "galay-tracing-t12-events-links.jsonl";
    std::filesystem::remove(path);

    galay::tracing::Span span("file", make_context());
    assert(span.add_event("file.event", {galay::tracing::span_attribute("file.attr", 7)}));
    assert(span.add_link(make_linked_context(), {}, {galay::tracing::span_attribute("link.attr", true)}));
    span.end();

    {
        galay::tracing::FileSpanExporter exporter(path);
        std::vector<galay::tracing::Span> spans;
        spans.push_back(std::move(span));
        assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
        assert(exporter.force_flush(std::chrono::milliseconds(0)));
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

void file_exporter_escapes_jsonl_control_characters() {
    const auto path = std::filesystem::temp_directory_path() / "galay-tracing-t12-control-chars.jsonl";
    std::filesystem::remove(path);

    galay::tracing::Span span("line\nname", make_context());
    assert(span.add_event("tab\tand\x01" "control", {galay::tracing::span_attribute("attr\nkey", "value\r\nnext")}));
    span.end();

    {
        galay::tracing::FileSpanExporter exporter(path);
        std::vector<galay::tracing::Span> spans;
        spans.push_back(std::move(span));
        assert(exporter.export_spans(std::span<const galay::tracing::Span>(spans)) == galay::tracing::ExportResult::kSuccess);
        assert(exporter.force_flush(std::chrono::milliseconds(0)));
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
    span_stores_bounded_events_with_attributes();
    span_stores_bounded_links_with_attributes();
    otlp_json_exporter_encodes_events_and_links();
    file_exporter_encodes_events_and_links();
    file_exporter_escapes_jsonl_control_characters();
}
