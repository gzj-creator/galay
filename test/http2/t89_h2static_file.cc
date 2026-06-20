/**
 * @file t89_h2static_file.cc
 * @brief HTTP/2 static file metadata/cache tests
 */

#include "galay-http2/server/h2_static_file.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace galay::http2;

namespace {

std::string headerValue(const H2StaticFileLookup& lookup, const std::string& name)
{
    for (const auto& header : lookup.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

} // namespace

int main()
{
    namespace fs = std::filesystem;

    const auto base = fs::temp_directory_path() /
        ("galay-h2-static-file-" + std::to_string(::getpid()));
    const auto root = base / "public";
    fs::create_directories(root);

    {
        std::ofstream(root / "hello.txt") << "hello";
        std::ofstream(base / "secret.txt") << "secret";
    }

    H2StaticFileCache cache(H2StaticFileConfig{
        .root = root,
        .small_file_threshold = 64 * 1024,
    });

    auto hit = cache.lookup(H2StaticFileRequest{.path = "/hello.txt"});
    assert(hit.status == 200);
    assert(hit.file_size == 5);
    assert(hit.content_type == "text/plain");
    assert(hit.body_cached);
    assert(hit.body && *hit.body == "hello");
    assert(!hit.etag.empty());
    assert(headerValue(hit, "content-length") == "5");
    assert(headerValue(hit, "content-type") == "text/plain");
    assert(headerValue(hit, "etag") == hit.etag);

    auto not_modified = cache.lookup(H2StaticFileRequest{
        .path = "/hello.txt",
        .if_none_match = hit.etag,
    });
    assert(not_modified.status == 304);
    assert(not_modified.file_size == 5);
    assert(not_modified.body == nullptr);
    assert(headerValue(not_modified, "etag") == hit.etag);

    auto missing = cache.lookup(H2StaticFileRequest{.path = "/missing.txt"});
    assert(missing.status == 404);
    assert(missing.body == nullptr);

    auto escaped = cache.lookup(H2StaticFileRequest{.path = "/../secret.txt"});
    assert(escaped.status == 404);
    assert(escaped.body == nullptr);

    fs::remove_all(base);
    std::cout << "t89_h2static_file PASS\n";
    return 0;
}
