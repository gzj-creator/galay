#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-mcp/common/mcp_json.h>
#include <galay/cpp/galay-etcd/base/etcd_internal.h>
#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>

#include <cassert>
#include <string>

namespace {
struct Endpoint {
    std::string host;
    int port{};
};
#define ENDPOINT_FIELDS(X) X(host) X(port)
REFLECT_FIELDS(Endpoint, ENDPOINT_FIELDS)
#undef ENDPOINT_FIELDS
}

int main() {
    const Endpoint config{"localhost", 8080};
    const auto text = json::serialize(config);
    assert(text);
    simdjson::dom::parser parser;
    simdjson::dom::element document;
    const auto error = parser.parse(*text).get(document);
    assert(error == simdjson::SUCCESS);
    std::int64_t port = 0;
    assert(document["port"].get(port) == simdjson::SUCCESS);
    assert(port == config.port);
    const auto mcp_document = galay::mcp::JsonDocument::parse(*text);
    assert(mcp_document);
    galay::mcp::JsonObject object;
    assert(galay::mcp::JsonHelper::getObject(mcp_document->root(), object));
    assert(galay::mcp::JsonHelper::getInt64(object, "port", port));
    assert(port == config.port);
    assert(!galay::mcp::JsonDocument::parse("{"));
}
