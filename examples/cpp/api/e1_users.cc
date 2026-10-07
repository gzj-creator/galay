#include <galay/cpp/galay-api/api_router.h>
#include <galay/cpp/galay-api/api_server.h>
#include <galay/cpp/galay-api/docs.h>
#include <serde/reflect/reflect_macros.hpp>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace galay::api;
using galay::http::HttpMethod;
using galay::kernel::Task;

namespace users {

struct GetUserInput {
    std::uint64_t id{};
    std::optional<bool> verbose;
};

struct CreateUserInput {
    std::string name;
    std::optional<std::string> email;
};

struct UserDto {
    std::uint64_t id{};
    std::string name;
    std::optional<std::string> email;
    std::optional<std::string> details;
};

constexpr reflect::field_options<std::uint64_t> id_options{
    .description = "User ID", .minimum = 1};
constexpr reflect::field_options<std::optional<bool>> verbose_options{
    .description = "Include profile details"};
constexpr reflect::field_options<std::string> name_options{
    .description = "Display name", .min_length = 1, .max_length = 40};
constexpr reflect::field_options<std::optional<std::string>> email_options{
    .description = "Contact email", .max_length = 254};

#define GET_USER_FIELDS(X) \
    X(id, "id", (id_options)) \
    X(verbose, "verbose", (verbose_options))
REFLECT_FIELDS(GetUserInput, GET_USER_FIELDS)
#undef GET_USER_FIELDS

#define CREATE_USER_FIELDS(X) \
    X(name, "name", (name_options)) \
    X(email, "email", (email_options))
REFLECT_FIELDS(CreateUserInput, CREATE_USER_FIELDS)
#undef CREATE_USER_FIELDS

#define USER_FIELDS(X) \
    X(id, "id", (id_options)) \
    X(name, "name", (name_options)) \
    X(email, "email", (email_options)) \
    X(details)
REFLECT_FIELDS(UserDto, USER_FIELDS)
#undef USER_FIELDS

Task<ApiResult<UserDto>> get_user(ApiContext&, GetUserInput input)
{
    if (input.id != 1 && input.id != 2) {
        co_return std::unexpected(ApiError{ApiErrorCode::kBusinessError, "User not found", 404});
    }
    UserDto user{.id = input.id,
                 .name = input.id == 1 ? "Ada" : "Grace",
                 .email = input.id == 1 ? "ada@example.test" : "grace@example.test"};
    if (input.verbose.value_or(false)) user.details = "Galay API example profile";
    co_return user;
}

Task<ApiResult<UserDto>> create_user(ApiContext&, CreateUserInput input)
{
    if (input.name == "admin") {
        co_return std::unexpected(ApiError{ApiErrorCode::kBusinessError, "Name is reserved", 409});
    }
    // The example has no database or mutable user store; applications supply
    // persistence in their own typed handler without changing the API contract.
    co_return UserDto{.id = 100, .name = std::move(input.name), .email = std::move(input.email)};
}

ApiResult<PreparedApi> prepare_api()
{
    ApiBuilder builder(ApiInfo{.title = "Users API", .version = "1.0.0",
                               .description = "Typed HTTP/1 users example"});
    auto get = builder.add<HttpMethod::GET, GetUserInput, UserDto>(
        "/users/:id", get_user,
        Operation{.id = "getUser", .summary = "Get a user", .tags = {"Users"},
                  .errors = {{404, "User not found"}}},
        InputBinding<GetUserInput>{}.path<&GetUserInput::id>("id")
                                   .query<&GetUserInput::verbose>("verbose"));
    if (!get) return std::unexpected(std::move(get.error()));
    auto post = builder.add<HttpMethod::POST, CreateUserInput, UserDto>(
        "/users", create_user,
        Operation{.id = "createUser", .summary = "Create a user", .tags = {"Users"},
                  .success_status = 201, .success_description = "User created",
                  .errors = {{409, "Name is reserved"}}});
    if (!post) return std::unexpected(std::move(post.error()));
    return builder.build();
}

} // namespace users

namespace {

struct Options {
    std::optional<std::string> export_path;
    std::uint16_t port = 8087;
    bool help = false;
};

std::expected<Options, std::string> parse_arguments(int argc, char** argv)
{
    Options options;
    bool port_set = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--help") {
            options.help = true;
            continue;
        }
        if (option != "--export" && option != "--port") {
            return std::unexpected("Unknown option: " + std::string(option));
        }
        if (index + 1 == argc || std::string_view(argv[index + 1]).empty()) {
            return std::unexpected("Missing value for " + std::string(option));
        }
        const std::string_view value = argv[++index];
        if (option == "--export") {
            if (options.export_path) return std::unexpected("Duplicate --export");
            options.export_path = value;
        } else {
            if (port_set) return std::unexpected("Duplicate --port");
            port_set = true;
            unsigned int port = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                port == 0 || port > 65535) {
                return std::unexpected("--port must be an integer from 1 to 65535");
            }
            options.port = static_cast<std::uint16_t>(port);
        }
    }
    return options;
}

ApiError export_error(std::string_view action, const std::string& path, int error)
{
    return {ApiErrorCode::kResourceError, std::string(action) + " " + path + ": " +
        std::error_code(error, std::generic_category()).message(), 500};
}

ApiResult<void> cleanup_export(int fd, const std::string& temporary, ApiError error)
{
    if (fd >= 0 && ::close(fd) != 0) {
        const auto close_error = errno;
        error.message += "; close also failed: " +
            std::error_code(close_error, std::generic_category()).message();
    }
    if (::unlink(temporary.c_str()) != 0) {
        const auto unlink_error = errno;
        error.message += "; temporary file cleanup also failed: " +
            std::error_code(unlink_error, std::generic_category()).message();
    }
    return std::unexpected(std::move(error));
}

ApiResult<void> export_document(const std::string& path, std::string_view bytes)
{
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) return std::unexpected(export_error("create", temporary, errno));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return cleanup_export(fd, temporary, export_error("write", temporary, errno));
        if (count == 0) {
            return cleanup_export(fd, temporary,
                {ApiErrorCode::kResourceError, "write made no progress: " + temporary, 500});
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(fd) != 0) return cleanup_export(-1, temporary, export_error("close", temporary, errno));
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        return cleanup_export(-1, temporary, export_error("rename", path, errno));
    }
    return {};
}

int report_error(const ApiError& error)
{
    std::cerr << api_error_name(error.code) << ": " << error.message << '\n';
    return 1;
}

volatile std::sig_atomic_t stopping = 0;

void request_stop(int)
{
    stopping = 1;
}

} // namespace

int main(int argc, char** argv)
{
    auto options = parse_arguments(argc, argv);
    if (!options) {
        std::cerr << options.error() << '\n';
        return 2;
    }
    if (options->help) {
        std::cout << "Usage: example_api_e1_users [--export <path>] [--port <1-65535>]\n";
        return 0;
    }
    auto api = users::prepare_api();
    if (!api) return report_error(api.error());

    // Export precedes ApiServer construction, docs installation, Runtime and listen.
    if (options->export_path) {
        auto exported = export_document(*options->export_path, *api->document);
        if (!exported) return report_error(exported.error());
        std::cout << "Exported OpenAPI to " << *options->export_path << '\n';
        return 0;
    }
    if (std::signal(SIGINT, request_stop) == SIG_ERR ||
        std::signal(SIGTERM, request_stop) == SIG_ERR) {
        std::cerr << "Cannot register shutdown signals\n";
        return 1;
    }
    ApiServer<HttpSwagger> server(galay::http::HttpServerBuilder()
        .host("127.0.0.1").port(options->port)
        .io_scheduler_count(1).parallel_scheduler_count(1).build_config());
    auto started = server.start(std::move(*api));
    if (!started) return report_error(started.error());
    std::cout << "Swagger UI: http://127.0.0.1:" << options->port << "/docs\n"
              << "OpenAPI: http://127.0.0.1:" << options->port << "/openapi.json\n" << std::flush;
    while (!stopping && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    server.stop();
    return 0;
}
