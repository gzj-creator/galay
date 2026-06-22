#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        std::cerr << "[mcp.naming] failed to open " << path << '\n';
        std::exit(2);
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string stripCommentsAndLiterals(std::string text)
{
    enum class State {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharLiteral,
    };

    State state = State::Code;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char current = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';

        switch (state) {
            case State::Code:
                if (current == '/' && next == '/') {
                    text[i] = ' ';
                    text[++i] = ' ';
                    state = State::LineComment;
                } else if (current == '/' && next == '*') {
                    text[i] = ' ';
                    text[++i] = ' ';
                    state = State::BlockComment;
                } else if (current == '\"') {
                    text[i] = ' ';
                    state = State::StringLiteral;
                    escaped = false;
                } else if (current == '\'') {
                    text[i] = ' ';
                    state = State::CharLiteral;
                    escaped = false;
                }
                break;
            case State::LineComment:
                if (current == '\n') {
                    state = State::Code;
                } else {
                    text[i] = ' ';
                }
                break;
            case State::BlockComment:
                if (current == '*' && next == '/') {
                    text[i] = ' ';
                    text[++i] = ' ';
                    state = State::Code;
                } else if (current != '\n') {
                    text[i] = ' ';
                }
                break;
            case State::StringLiteral:
                if (current != '\n') {
                    text[i] = ' ';
                }
                if (!escaped && current == '\"') {
                    state = State::Code;
                }
                escaped = !escaped && current == '\\';
                break;
            case State::CharLiteral:
                if (current != '\n') {
                    text[i] = ' ';
                }
                if (!escaped && current == '\'') {
                    state = State::Code;
                }
                escaped = !escaped && current == '\\';
                break;
        }
    }
    return text;
}

bool containsUpperCamelFunction(const std::filesystem::path& path)
{
    static const std::regex kUpperCamelFunction(
        R"((^|[^A-Za-z0-9_:~])([A-Z][A-Za-z0-9_]*)\s*\()",
        std::regex::ECMAScript);
    static const std::regex kDeclaredType(
        R"(\b(?:class|struct|enum\s+class)\s+(?:[A-Za-z_][A-Za-z0-9_]*::)*([A-Z][A-Za-z0-9_]*)\b)",
        std::regex::ECMAScript);
    static const std::regex kIgnoredName(
        R"(^(Mcp[A-Z].*|Json[A-Z].*|Schema[A-Z].*|Prompt[A-Z].*|Content$|Resource$|Tool$|Methods$|MCP_.*|GALAY_.*)$)",
        std::regex::ECMAScript);

    const auto text = stripCommentsAndLiterals(readAll(path));
    std::unordered_set<std::string> declaredTypes;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), kDeclaredType);
         it != std::sregex_iterator();
         ++it) {
        declaredTypes.insert((*it)[1].str());
    }

    auto begin = std::sregex_iterator(text.begin(), text.end(), kUpperCamelFunction);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string name = (*it)[2].str();
        if (declaredTypes.contains(name) || std::regex_match(name, kIgnoredName)) {
            continue;
        }
        std::cerr << "[mcp.naming] upper camel function-like name " << name
                  << " in " << path << '\n';
        return true;
    }
    return false;
}

bool isCxxSource(const std::filesystem::path& path)
{
    const auto extension = path.extension().string();
    return extension == ".h" || extension == ".cc" ||
        extension == ".hpp" || extension == ".cppm";
}

} // namespace

int main()
{
    const auto root = std::filesystem::path(GALAY_PROJECT_SOURCE_DIR) /
        "src" / "cpp" / "galay-mcp";

    bool failed = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || !isCxxSource(entry.path())) {
            continue;
        }
        failed = containsUpperCamelFunction(entry.path()) || failed;
    }

    return failed ? 1 : 0;
}
