#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifndef GALAY_MONGO_SOURCE_DIR
#define GALAY_MONGO_SOURCE_DIR "."
#endif

namespace
{

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        std::cerr << "failed to open " << path << '\n';
        std::exit(1);
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool containsWord(const std::string& text, const std::string& word)
{
    size_t pos = text.find(word);
    while (pos != std::string::npos) {
        const bool left_ok = pos == 0 ||
            (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_');
        const size_t after = pos + word.size();
        const bool right_ok = after >= text.size() ||
            (!std::isalnum(static_cast<unsigned char>(text[after])) && text[after] != '_');
        if (left_ok && right_ok) {
            return true;
        }
        pos = text.find(word, pos + word.size());
    }
    return false;
}

} // namespace

int main()
{
    const std::filesystem::path source_root = GALAY_MONGO_SOURCE_DIR;
    const std::vector<std::filesystem::path> checked_files = {
        source_root / "galay-mongo" / "base" / "mongo_value.h",
        source_root / "galay-mongo" / "base" / "mongo_value.cc",
        source_root / "galay-mongo" / "protoc" / "bson.h",
        source_root / "galay-mongo" / "protoc" / "bson.cc",
        source_root / "galay-mongo" / "protoc" / "mongo_protocol.h",
        source_root / "galay-mongo" / "protoc" / "mongo_protocol.cc",
        source_root / "galay-mongo" / "protoc" / "builder.h",
        source_root / "galay-mongo" / "protoc" / "builder.cc",
    };

    bool failed = false;
    for (const auto& path : checked_files) {
        const std::string text = readAll(path);
        if (containsWord(text, "throw")) {
            std::cerr << path << ": Mongo public/protocol boundary must not throw\n";
            failed = true;
        }
        if (text.find("@throws") != std::string::npos) {
            std::cerr << path << ": Mongo public docs must not advertise throwing\n";
            failed = true;
        }
    }

    if (failed) {
        return 1;
    }

    std::cout << "T13-MongoExpectedErrorSource PASS\n";
    return 0;
}
