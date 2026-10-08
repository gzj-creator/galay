#include <galay/cpp/galay-api/docs.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <unistd.h>

namespace {

void require(bool condition, std::string_view message)
{
    if (condition) return;
    std::cerr << "api.embedded: " << message << '\n';
    std::exit(1);
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    std::array<char, 40> directory{};
    constexpr std::string_view pattern = "/tmp/galay-api-embedded-XXXXXX";
    require(pattern.copy(directory.data(), pattern.size()) == pattern.size(), "copy directory template");
    require(::mkdtemp(directory.data()) != nullptr, "create empty deployment directory");
    std::error_code error;
    const auto previous = fs::current_path(error);
    require(!error, "read working directory");
    fs::current_path(directory.data(), error);
    require(!error, "switch to empty deployment directory");

    galay::api::PreparedApi prepared;
    prepared.document = std::make_shared<const std::string>(
        R"({"openapi":"3.1.0","info":{"title":"Embedded","version":"1"},"paths":{}})");
    const auto installed = galay::api::HttpSwagger{galay::api::DocsConfig{}}.install(prepared);
    fs::current_path(previous, error);
    require(!error, "restore working directory");
    require(fs::remove(directory.data(), error) && !error, "remove empty deployment directory");
    if (!installed) std::cerr << installed.error().message << '\n';
    require(installed.has_value(), "default Swagger must not require a resource directory");
    require(prepared.docs_installed, "default Swagger marks document routes installed");
    constexpr std::array names{
        "swagger-ui.css", "swagger-ui-bundle.js", "swagger-ui-standalone-preset.js",
        "favicon-16x16.png", "favicon-32x32.png", "LICENSE", "NOTICE", "README.md", "SHA256SUMS"};
    for (const auto name : names) {
        require(prepared.router.find_handler(galay::http::HttpMethod::GET,
                    std::string("/docs/") + name).request_handler != nullptr,
                "embedded UI and provenance routes must exist without deployed files");
    }
    require(prepared.router.find_handler(galay::http::HttpMethod::GET, "/openapi.json").request_handler != nullptr,
            "default Swagger installs the REST document");
    std::cout << "Embedded UI installs from an empty working directory\n";
}
