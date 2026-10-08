#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-api/api_router.h>
#include <serde/json/json.hpp>
#include <serde/reflect/reflect_macros.hpp>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace consumer {
struct Input { std::uint64_t id{}; std::optional<bool> verbose; };
struct Output { std::uint64_t id{}; std::optional<std::string> label; };
#define INPUT_FIELDS(X) X(id) X(verbose)
REFLECT_FIELDS(Input, INPUT_FIELDS)
#undef INPUT_FIELDS
#define OUTPUT_FIELDS(X) X(id) X(label)
REFLECT_FIELDS(Output, OUTPUT_FIELDS)
#undef OUTPUT_FIELDS

galay::kernel::Task<galay::api::ApiResult<Output>> get(galay::api::ApiContext&, Input input) {
    co_return Output{input.id, std::nullopt};
}

galay::kernel::Task<void> raw(galay::http::HttpConn&, galay::http::HttpRequest) {
    co_return;
}

void require(bool condition, std::string_view message) {
    if (condition) return;
    if (std::fprintf(stderr, "installed.api: %.*s\n", static_cast<int>(message.size()), message.data()) < 0) {
        std::exit(2);
    }
    std::exit(1);
}

static_assert(!std::is_copy_constructible_v<galay::http::HttpServerBuilder<>>);
static_assert(!std::is_move_constructible_v<galay::http::HttpServer>);
} // namespace consumer

template<class Builder>
galay::api::ApiResult<void> register_items(Builder& builder)
{
    using namespace galay::api;
    return builder.template add_api<galay::http::HttpMethod::GET, consumer::Input, consumer::Output>(
        "/items/:id", consumer::get, Operation{.id = "getItem"},
        InputBinding<consumer::Input>{}.path<&consumer::Input::id>("id")
            .query<&consumer::Input::verbose>("verbose"));
}

int main() {
    using namespace galay::api;
    using consumer::require;
    galay::http::HttpServerBuilder<true> builder;
    builder.io_scheduler_count(1).parallel_scheduler_count(1);
    builder.api_info({.title = "Installed API", .version = "1.0.0"});
    require(register_items(builder).has_value(), "installed typed registration");
    require(builder.add_handler<galay::http::HttpMethod::GET>("/healthz", consumer::raw).has_value(), "raw route");
    const auto document = builder.export_openapi();
    require(document.has_value(), "installed offline document");
    const auto parsed = json::parse(*document);
    require(parsed.has_value(), "installed document parses");
    const auto version = (*parsed)["openapi"].as_string();
    require(version && *version == "3.1.0", "installed OpenAPI version");
    const auto limit = parsed->at("paths").at("/items/{id}").at("get").at("parameters")
        .at(0).at("schema").at("maximum").as_uint64();
    require(limit && *limit == std::numeric_limits<std::uint64_t>::max(), "exact uint64 limit");
    require((*parsed)["paths"].size() == 1 && !(*parsed)["paths"].contains("/healthz"), "raw routes stay undocumented");
    auto visible = builder.build();
    require(visible && !(*visible)->is_running(), "native construction does not listen");
    galay::http::HttpServerBuilder<> hidden;
    hidden.io_scheduler_count(1).parallel_scheduler_count(1);
    require(register_items(hidden).has_value() && hidden.build().has_value(), "same registration without Swagger");
    require(std::puts("Installed native server builder consumer passed") >= 0, "write result");
}
