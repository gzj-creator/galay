#include <galay/cpp/galay-utils/algorithm/consistent_hash.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

bool source_does_not_use_blocking_locks()
{
    std::ifstream input(
        std::string(GALAY_UTILS_SOURCE_DIR) + "/galay-utils/algorithm/consistent_hash.hpp");
    if (!input.is_open()) {
        std::cerr << "failed to open consistent_hash.hpp source\n";
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();
    const char* forbidden[] = {
        "<mutex>",
        "<shared_mutex>",
        "std::mutex",
        "std::shared_mutex",
        "std::unique_lock",
        "std::shared_lock",
    };
    for (const char* token : forbidden) {
        if (source.find(token) != std::string::npos) {
            std::cerr << "consistent_hash.hpp must not use blocking lock token: "
                      << token << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    if (!source_does_not_use_blocking_locks()) {
        return 1;
    }

    galay::utils::ConsistentHash ring;
    ring.add_node({"node-a", "127.0.0.1:9000", 1});

    const auto node = ring.get_node("request-a");
    if (!node.has_value() || node->id != "node-a") {
        std::cerr << "consistent_hash header self-contained smoke failed\n";
        return 1;
    }
    return 0;
}
