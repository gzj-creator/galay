#include <galay/cpp/galay-etcd/base/etcd_internal.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::filesystem::path repoRoot()
{
    std::filesystem::path file = __FILE__;
    return file.parent_path().parent_path().parent_path().parent_path();
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

int expectParsed(
    const std::string& endpoint,
    const std::string& host,
    uint16_t port,
    bool secure,
    bool ipv6)
{
    auto parsed = galay::etcd::internal::parseEndpoint(endpoint);
    if (!parsed.has_value()) {
        return fail("parseEndpoint rejected " + endpoint + ": " + parsed.error());
    }
    if (parsed->host != host) {
        return fail("parseEndpoint host mismatch for " + endpoint);
    }
    if (parsed->port != port) {
        return fail("parseEndpoint port mismatch for " + endpoint);
    }
    if (parsed->secure != secure) {
        return fail("parseEndpoint secure flag mismatch for " + endpoint);
    }
    if (parsed->ipv6 != ipv6) {
        return fail("parseEndpoint ipv6 flag mismatch for " + endpoint);
    }
    return 0;
}

int expectRejected(const std::string& endpoint, const std::string& error_fragment)
{
    auto parsed = galay::etcd::internal::parseEndpoint(endpoint);
    if (parsed.has_value()) {
        return fail("parseEndpoint accepted invalid endpoint " + endpoint);
    }
    if (!contains(parsed.error(), error_fragment)) {
        return fail("parseEndpoint error mismatch for " + endpoint + ": " + parsed.error());
    }
    return 0;
}

} // namespace

int main()
{
    if (const int rc = expectParsed("http://127.0.0.1:2379", "127.0.0.1", 2379, false, false);
        rc != 0) {
        return rc;
    }
    if (const int rc = expectParsed("https://etcd.example.com:1234", "etcd.example.com", 1234, true, false);
        rc != 0) {
        return rc;
    }
    if (const int rc = expectRejected("127.0.0.1:2379", "invalid endpoint:"); rc != 0) {
        return rc;
    }
    if (const int rc = expectRejected("http://127.0.0.1:0", "endpoint port out of range:"); rc != 0) {
        return rc;
    }
    if (const int rc = expectRejected("http://127.0.0.1:65536", "endpoint port out of range:"); rc != 0) {
        return rc;
    }
    if (const int rc = expectRejected("http://127.0.0.1:abc", "invalid endpoint:"); rc != 0) {
        return rc;
    }
    if (const int rc = expectRejected("http://:2379", "invalid endpoint:"); rc != 0) {
        return rc;
    }

    const std::string internal_header = readFile(repoRoot() / "src/cpp/galay-etcd/base/etcd_internal.h");
    if (internal_header.empty()) {
        return fail("failed to read etcd_internal.h");
    }
    if (contains(internal_header, "#include <regex>") ||
        contains(internal_header, "std::regex") ||
        contains(internal_header, "regex_match")) {
        return fail("endpoint parser should use direct string parsing instead of std::regex");
    }

    std::cout << "T15-EtcdEndpointParser PASS\n";
    return 0;
}
