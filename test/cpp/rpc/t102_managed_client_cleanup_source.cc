#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot()
{
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string trim(std::string line)
{
    const auto begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = line.find_last_not_of(" \t\r\n");
    return line.substr(begin, end - begin + 1);
}

bool hasBareCloseAwait(const std::string& source)
{
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        if (trim(line) == "co_await client.close();") {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    const auto source_path = projectRoot() / "src/cpp/galay-rpc/kernel/rpc_managed_client.h";
    const auto source = readAll(source_path);
    if (source.empty()) {
        std::cerr << "failed to read " << source_path << "\n";
        return 1;
    }

    std::vector<std::string> failures;
    if (source.find("(void)release_result") != std::string::npos) {
        failures.push_back("managed client release() result is explicitly discarded");
    }
    if (hasBareCloseAwait(source)) {
        failures.push_back("managed client close() result is awaited without handling");
    }

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << failure << "\n";
        }
        return 1;
    }

    std::cout << "RPC managed client cleanup source PASS\n";
    return 0;
}
