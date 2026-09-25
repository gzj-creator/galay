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
    json::Parser parser;
    const auto document = parser.parse(*text);
    assert(document);
    const auto port = document->at("port").as_int64();
    assert(port);
    assert(*port == config.port);
    const auto mcp_document = galay::mcp::JsonDocument::parse(*text);
    assert(mcp_document);
    const json::Json& object = mcp_document->root();
    assert(object.is_object());
    const auto mcp_port = object.at("port").as_int64();
    assert(mcp_port);
    assert(*mcp_port == config.port);
    assert(*mcp_port == *port);
    assert(!galay::mcp::JsonDocument::parse("{"));
}
