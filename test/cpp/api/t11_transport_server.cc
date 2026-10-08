#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-http/server/http_server.h>
#ifdef GALAY_HTTP2_FEATURE_ENABLED
#include <galay/cpp/galay-http2/server/http2_server.h>
#endif
#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <serde/reflect/reflect_macros.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace galay::api;
using galay::http::HttpMethod;
using galay::kernel::Task;

namespace fixture {

struct GetInput {
    std::uint64_t id{};
    std::optional<bool> verbose = true;
};
struct PostInput {
    std::uint64_t id{};
    std::optional<bool> verbose = true;
    std::string title;
};
struct Output {
    std::uint64_t id{};
    std::string title;
    bool verbose{};
};
struct Stats {
    int started{};
    int completed{};
    int active{};
    int peak{};
    int posts{};
};

constexpr reflect::field_options<std::uint64_t> id_options{.minimum = 1};
constexpr reflect::field_options<std::string> title_options{.min_length = 1, .max_length = 40};
#define GET_FIELDS(X) X(id, "id", (id_options)) X(verbose)
REFLECT_FIELDS(GetInput, GET_FIELDS)
#undef GET_FIELDS
#define POST_FIELDS(X) X(id, "id", (id_options)) X(verbose) X(title, "display-title", (title_options))
REFLECT_FIELDS(PostInput, POST_FIELDS)
#undef POST_FIELDS
#define OUTPUT_FIELDS(X) X(id) X(title) X(verbose)
REFLECT_FIELDS(Output, OUTPUT_FIELDS)
#undef OUTPUT_FIELDS
#define STATS_FIELDS(X) X(started) X(completed) X(active) X(peak) X(posts)
REFLECT_FIELDS(Stats, STATS_FIELDS)
#undef STATS_FIELDS

std::atomic<int> started{0};
std::atomic<int> completed{0};
std::atomic<int> active{0};
std::atomic<int> peak{0};
std::atomic<int> posts{0};

Task<ApiResult<Output>> get_value(ApiContext&, GetInput input)
{
    const auto runtime = galay::kernel::RuntimeHandle::current();
    if (!runtime || !runtime->is_valid()) {
        co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "missing handler runtime", 500});
    }
    co_return Output{input.id, "Ada", input.verbose.value_or(false)};
}

Task<ApiResult<Output>> post_value(ApiContext&, PostInput input)
{
    const int previous = posts.fetch_add(1);
    if (previous < 0) co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "counter overflow", 500});
    co_return Output{input.id, std::move(input.title), input.verbose.value_or(false)};
}

Task<ApiResult<Output>> slow_value(ApiContext& context, GetInput input)
{
    const int count = active.fetch_add(1) + 1;
    const int previous_started = started.fetch_add(1);
    if (previous_started < 0) co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "counter overflow", 500});
    int observed = peak.load();
    while (observed < count && !peak.compare_exchange_weak(observed, count)) {}
    galay::kernel::AsyncWaiter<void> timer;
    const auto slept = co_await timer.wait().timeout(std::chrono::milliseconds(150));
    const int previous_active = active.fetch_sub(1);
    const int previous_completed = completed.fetch_add(1);
    const auto runtime = galay::kernel::RuntimeHandle::current();
    if (!runtime || !runtime->is_valid()) {
        co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "missing suspended handler runtime", 500});
    }
    if (slept || !galay::kernel::IOError::contains(slept.error().code(), galay::kernel::kTimeout) || previous_active < 1 || previous_completed < 0) {
        co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "sleep or counter failure", 500});
    }
    if (!context.request.body_str().empty()) {
        co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "request changed during suspension", 500});
    }
    co_return Output{input.id, "suspended", input.verbose.value_or(false)};
}

Task<ApiResult<std::string_view>> borrowed_value(ApiContext& context, PostInput)
{
    galay::kernel::AsyncWaiter<void> timer;
    const auto slept = co_await timer.wait().timeout(std::chrono::milliseconds(10));
    if (slept || !galay::kernel::IOError::contains(slept.error().code(), galay::kernel::kTimeout)) {
        co_return std::unexpected(ApiError{ApiErrorCode::kTaskError, "sleep failed", 500});
    }
    co_return std::string_view(context.request.body_str());
}

Task<ApiResult<Stats>> statistics(ApiContext&, NoInput)
{
    co_return Stats{started.load(), completed.load(), active.load(), peak.load(), posts.load()};
}

Task<ApiResult<Output>> business_error(ApiContext&, NoInput)
{
    co_return std::unexpected(ApiError{ApiErrorCode::kBusinessError, "not found", 404});
}

Task<ApiResult<Output>> undeclared_error(ApiContext&, NoInput)
{
    co_return std::unexpected(ApiError{ApiErrorCode::kBusinessError, "undeclared", 409});
}

Task<ApiResult<double>> encoding_error(ApiContext&, NoInput)
{
    co_return std::numeric_limits<double>::infinity();
}

Task<ApiResult<NoContent>> empty_value(ApiContext&, NoInput)
{
    co_return NoContent{};
}

template<class Builder>
ApiResult<void> register_api(Builder& builder)
{
    const auto add_get = [&]<HttpMethod Method>(std::string path, std::string id, auto handler) {
        return builder.template add_api<Method, GetInput, Output>(std::move(path), handler, Operation{.id = std::move(id)},
            InputBinding<GetInput>{}.path<&GetInput::id>("id").query<&GetInput::verbose>("verbose"));
    };
    if (auto added = add_get.template operator()<HttpMethod::GET>("/values/:id", "getValue", get_value); !added) return std::unexpected(added.error());
    if (auto added = add_get.template operator()<HttpMethod::GET>("/slow/:id", "slowValue", slow_value); !added) return std::unexpected(added.error());
    if (auto added = add_get.template operator()<HttpMethod::HEAD>("/head/:id", "headValue", get_value); !added) return std::unexpected(added.error());
    const auto binding = InputBinding<PostInput>{}.path<&PostInput::id>("id").query<&PostInput::verbose>("verbose");
    if (auto added = builder.template add_api<HttpMethod::POST, PostInput, Output>("/values/:id", post_value,
        Operation{.id = "postValue", .success_status = 201}, binding); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::POST, PostInput, std::string_view>("/borrowed/:id", borrowed_value,
        Operation{.id = "borrowedValue"}, binding); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::GET, NoInput, Stats>("/stats", statistics,
        Operation{.id = "getStats"}); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::GET, NoInput, Output>("/business", business_error,
        Operation{.id = "businessError", .errors = {{404, "not found"}}}); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::GET, NoInput, Output>("/undeclared", undeclared_error,
        Operation{.id = "undeclaredError"}); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::GET, NoInput, double>("/encoding-error", encoding_error,
        Operation{.id = "encodingError"}); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::GET, NoInput, NoContent>("/no-content", empty_value,
        Operation{.id = "noContent", .success_status = 204}); !added) return std::unexpected(added.error());
    if (auto added = builder.template add_api<HttpMethod::POST, NoInput, NoContent>("/reset-content", empty_value,
        Operation{.id = "resetContent", .success_status = 205}); !added) return std::unexpected(added.error());
    return {};
}

} // namespace fixture

namespace {

class TestLogger final : public galay::kernel::BaseLogger {
public:
    void log(galay::kernel::LogLevel, std::string_view tag, std::string_view message,
             const char*, int, const char*) override
    {
        const int written = std::fprintf(stderr, "%.*s %.*s\n", static_cast<int>(tag.size()), tag.data(),
            static_cast<int>(message.size()), message.data());
        if (written < 0) std::exit(1);
    }
    galay::kernel::LogLevel min_level() const override { return galay::kernel::LogLevel::kWarn; }
};

volatile std::sig_atomic_t stopping = 0;
void request_stop(int) { stopping = 1; }

int report_error(const ApiError& error)
{
    std::cerr << api_error_name(error.code) << ": " << error.message << '\n';
    return 1;
}

template<class Builder>
int run_server(Builder& builder, const std::string& directory)
{
    if constexpr (requires { builder.docs(); }) {
        builder.api_info({.title = "Transport acceptance", .version = "1"});
        if (!directory.empty()) builder.docs({}, directory);
    }
    const auto registered = fixture::register_api(builder);
    if (!registered) return report_error(registered.error());
    auto built = builder.build();
    if (!built) return report_error(built.error());
    auto& server = **built;
    const auto started = server.start();
    if (!started) return report_error(started.error());
    std::cout << "READY\n" << std::flush;
    while (!stopping && server.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    server.stop();
    server.stop();
    if (fixture::active.load() != 0 || fixture::completed.load() != fixture::started.load()) {
        std::cerr << "stop returned with an unfinished typed handler\n";
        return 1;
    }
    return 0;
}

template<bool EnableSwagger>
int select_transport(const std::string& transport, std::uint16_t port,
                     const std::string& certificate, const std::string& key, const std::string& directory)
{
    if (transport == "http") {
        galay::http::HttpServerBuilder<EnableSwagger> builder;
        builder.host("127.0.0.1").port(port).io_scheduler_count(2).parallel_scheduler_count(1);
        return run_server(builder, directory);
    }
#ifdef GALAY_SSL_FEATURE_ENABLED
    if (transport == "https") {
        galay::http::HttpsServerBuilder<EnableSwagger> builder;
        builder.host("127.0.0.1").port(port).cert_path(certificate).key_path(key)
            .io_scheduler_count(2).parallel_scheduler_count(1);
        return run_server(builder, directory);
    }
#endif
#ifdef GALAY_HTTP2_FEATURE_ENABLED
    if (transport == "h2c") {
        galay::http2::H2cServerBuilder<EnableSwagger> builder;
        builder.host("127.0.0.1").port(port).io_scheduler_count(2).parallel_scheduler_count(1).ping_enabled(false);
        return run_server(builder, directory);
    }
#ifdef GALAY_SSL_FEATURE_ENABLED
    if (transport == "h2") {
        galay::http2::H2ServerBuilder<EnableSwagger> builder;
        builder.host("127.0.0.1").port(port).cert_path(certificate).key_path(key)
            .io_scheduler_count(2).parallel_scheduler_count(1).ping_enabled(false);
        return run_server(builder, directory);
    }
#endif
#endif
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    galay::http::log::set(std::make_unique<TestLogger>());
    std::string transport = "http";
    std::string certificate, key, directory, export_path;
    std::uint16_t port = 0;
    bool no_swagger = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--no-swagger") { no_swagger = true; continue; }
        if (index + 1 == argc) return 2;
        const std::string_view value = argv[++index];
        if (option == "--transport") transport = value;
        else if (option == "--cert") certificate = value;
        else if (option == "--key") key = value;
        else if (option == "--assets") directory = value;
        else if (option == "--export") export_path = value;
        else if (option == "--port") {
            unsigned int number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || number == 0 || number > 65535) return 2;
            port = static_cast<std::uint16_t>(number);
        } else return 2;
    }
    if (!export_path.empty()) {
        galay::http::HttpServerBuilder<true> builder;
        builder.api_info({.title = "Transport acceptance", .version = "1"});
        const auto registered = fixture::register_api(builder);
        if (!registered) return report_error(registered.error());
        const auto document = builder.export_openapi();
        if (!document) return report_error(document.error());
        auto* file = std::fopen(export_path.c_str(), "wb");
        if (!file) return 1;
        const bool written = std::fwrite(document->data(), 1, document->size(), file) == document->size();
        const bool closed = std::fclose(file) == 0;
        return written && closed ? 0 : 1;
    }
    if (!port || std::signal(SIGINT, request_stop) == SIG_ERR || std::signal(SIGTERM, request_stop) == SIG_ERR) return 2;
    return no_swagger ? select_transport<false>(transport, port, certificate, key, directory)
                      : select_transport<true>(transport, port, certificate, key, directory);
}
