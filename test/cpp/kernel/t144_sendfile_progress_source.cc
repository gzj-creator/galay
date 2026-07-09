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
        return {};
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
    const auto awaitable_header =
        readFile(root / "src/cpp/galay-kernel/core/awaitable.h");
    const auto awaitable_inline =
        readFile(root / "src/cpp/galay-kernel/core/awaitable.inl");

    if (awaitable_header.empty() || awaitable_inline.empty()) {
        return 1;
    }

    if (!contains(awaitable_header, "m_transferred")) {
        std::cerr << "sendfile awaitable must track cumulative bytes across readiness events\n";
        return 1;
    }

    for (const auto* required : {
             "while (m_count > 0)",
             "m_offset += static_cast<off_t>(sent)",
             "m_count -= sent",
             "m_transferred += sent",
             "IOError::contains(result.error().code(), kNotReady)",
         }) {
        if (!contains(awaitable_inline, required)) {
            std::cerr << "sendfile awaitable must advance progress before retrying: "
                      << required << "\n";
            return 1;
        }
    }

    std::cout << "T144-SendFileProgressSource PASS\n";
    return 0;
}
