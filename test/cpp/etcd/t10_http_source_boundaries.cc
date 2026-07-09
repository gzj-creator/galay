#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        std::cerr << "failed to open " << path << "\n";
        std::exit(1);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
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

} // namespace

int main()
{
    const auto root = repoRoot();
    const auto sync_client = readFile(root / "src/cpp/galay-etcd/sync/etcd_client.cc");
    const auto async_client = readFile(root / "src/cpp/galay-etcd/async/client.cc");
    const auto internal_header = readFile(root / "src/cpp/galay-etcd/base/etcd_internal.h");

    for (const auto* required : {
             "chunk size overflows buffer bounds",
             "std::numeric_limits<size_t>::max() - 2",
         }) {
        if (!contains(sync_client, required) || !contains(async_client, required)) {
            std::cerr << "etcd chunk decoder must guard overflow: " << required << "\n";
            return 1;
        }
    }

    if (!contains(async_client, "kMaxWatchHeaderBytes") ||
        !contains(async_client, "watch response header too large")) {
        std::cerr << "etcd watch worker must enforce a hard response-header limit\n";
        return 1;
    }

    if (!contains(internal_header, "containsAsciiTokenIgnoreCase")) {
        std::cerr << "etcd HTTP parser token helper must live in shared internal header\n";
        return 1;
    }
    if (contains(sync_client, "bool containsAsciiTokenIgnoreCase") ||
        contains(async_client, "bool containsAsciiTokenIgnoreCase")) {
        std::cerr << "etcd HTTP parser token helper must not be duplicated in clients\n";
        return 1;
    }
    if (!contains(sync_client, "containsAsciiTokenIgnoreCase") ||
        !contains(async_client, "containsAsciiTokenIgnoreCase")) {
        std::cerr << "etcd HTTP parser must recognize comma-separated header tokens case-insensitively\n";
        return 1;
    }
    if (contains(sync_client, "value.find(\"chunked\")") ||
        contains(async_client, "value.find(\"chunked\")")) {
        std::cerr << "etcd transfer-encoding parser must not use case-sensitive substring matching\n";
        return 1;
    }

    if (!contains(async_client, "joinWatchWorkers()")) {
        std::cerr << "etcd watch close source boundary should keep join isolated for future refactor\n";
        return 1;
    }

    std::cout << "T10-EtcdHttpSourceBoundaries PASS\n";
    return 0;
}
