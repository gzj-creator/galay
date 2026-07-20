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
    const auto sync_client = readFile(root / "src/cpp/galay-mysql/sync/mysql_client.cc");
    const auto async_awaitable = readFile(root / "src/cpp/galay-mysql/details/awaitable.inl");

    for (const auto* capability : {
             "CLIENT_MULTI_STATEMENTS",
             "CLIENT_MULTI_RESULTS",
             "CLIENT_PS_MULTI_RESULTS",
         }) {
        if (!contains(sync_client, capability) || !contains(async_awaitable, capability)) {
            std::cerr << "mysql clients must keep multi-result capability: " << capability << "\n";
            return 1;
        }
    }

    if (!contains(async_awaitable, "MysqlPipelineAwaitable<Strategy>::Machine::finalizeCurrentResult") ||
        !contains(async_awaitable, "m_state->results.push_back")) {
        std::cerr << "async pipeline must preserve per-response result boundaries\n";
        return 1;
    }

    std::cout << "T18-MySQLMultiResultSource PASS\n";
    return 0;
}
