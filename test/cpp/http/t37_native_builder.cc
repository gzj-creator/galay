#include <galay/cpp/galay-http/server/http_server.h>

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message)
{
    if (condition) return;
    if (std::fprintf(stderr, "%s\n", message) < 0) std::abort();
    std::exit(1);
}

galay::kernel::Task<galay::http::HttpResponseResult> get_value(galay::http::HttpRequest)
{
    galay::http::HttpResponse response;
    response.set_body_str("plain HTTP");
    co_return response;
}

} // namespace

int main()
{
    using namespace galay::http;
    HttpServerBuilder<> builder;
    builder.host("127.0.0.1").port(0).io_scheduler_count(1).parallel_scheduler_count(1);
    require(builder.add_request_handler<HttpMethod::GET>("/value", get_value).has_value(),
        "ordinary routes require no API or serde dependency");
    const auto duplicate = builder.add_request_handler<HttpMethod::GET>("/value", get_value);
    require(!duplicate && duplicate.error().code == galay::api::ApiErrorCode::kRouteConflict,
        "ordinary duplicate route is explicit");
    auto built = builder.build();
    require(built && !(*built)->is_running(), "native builder returns a stopped server");
    require((*built)->start().has_value(), "plain native server starts");
    require(!(*built)->start(), "managed startup is single-use");
    (*built)->stop();
    (*built)->stop();
    require(!builder.build(), "successful build freezes registration");
}
