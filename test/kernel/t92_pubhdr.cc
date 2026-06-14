/**
 * @file t92_pubhdr.cc
 * @brief 用途：验证公开 async 头文件不会把 `galay::kernel` 整体导出进 `galay::async`。
 * 关键覆盖点：安装面的头文件 hygiene、`using namespace` 污染、consumer 可见命名空间边界。
 * 通过条件：公开 async 头文件中不存在 `using namespace galay::kernel;`。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot()
{
    auto path = std::filesystem::path(__FILE__).parent_path().parent_path();
    return path.lexically_normal();
}

bool fileContainsPattern(const std::filesystem::path& path, const std::string& pattern)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.find(pattern) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    const auto root = projectRoot();
    const std::vector<std::filesystem::path> public_headers = {
        root / "galay-kernel" / "async" / "tcp_socket.h",
        root / "galay-kernel" / "async" / "udp_socket.h",
        root / "galay-kernel" / "async" / "file_watcher.h",
        root / "galay-kernel" / "async" / "async_file.h",
        root / "galay-kernel" / "async" / "aio_file.h",
    };

    std::vector<std::filesystem::path> polluted_headers;
    for (const auto& header : public_headers) {
        if (fileContainsPattern(header, "using namespace galay::kernel;")) {
            polluted_headers.push_back(header.lexically_relative(root));
        }
    }

    if (!polluted_headers.empty()) {
        std::cerr << "public async headers should not contain 'using namespace galay::kernel;':\n";
        for (const auto& header : polluted_headers) {
            std::cerr << "  - " << header.string() << '\n';
        }
        return 1;
    }

    std::cout << "T92-AsyncPublicHeaderHygiene PASS\n";
    return 0;
}
