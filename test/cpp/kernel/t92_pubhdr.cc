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
    return std::filesystem::path(GALAY_SOURCE_ROOT);
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
        root / "galay-kernel" / "async" / "async_tcp.h",
        root / "galay-kernel" / "async" / "async_udp.h",
        root / "galay-kernel" / "async" / "async_file_watcher.h",
        root / "galay-kernel" / "async" / "async_file.h",
        root / "galay-kernel" / "async" / "async_aio.h",
        root / "galay-kernel" / "async" / "async_mutex.h",
        root / "galay-kernel" / "async" / "async_waiter.h",
    };
    const std::vector<std::filesystem::path> legacy_headers = {
        root / "galay-kernel" / "async" / "tcp_socket.h",
        root / "galay-kernel" / "async" / "udp_socket.h",
        root / "galay-kernel" / "async" / "file_watcher.h",
        root / "galay-kernel" / "async" / "aio_file.h",
        root / "galay-kernel" / "concurrency" / "async_mutex.h",
        root / "galay-kernel" / "concurrency" / "async_waiter.h",
    };

    std::vector<std::filesystem::path> missing_headers;
    std::vector<std::filesystem::path> polluted_headers;
    for (const auto& header : public_headers) {
        if (!std::filesystem::is_regular_file(header)) {
            missing_headers.push_back(header.lexically_relative(root));
            continue;
        }
        if (fileContainsPattern(header, "using namespace galay::kernel;")) {
            polluted_headers.push_back(header.lexically_relative(root));
        }
    }

    std::vector<std::filesystem::path> remaining_legacy_headers;
    for (const auto& header : legacy_headers) {
        if (std::filesystem::exists(header)) {
            remaining_legacy_headers.push_back(header.lexically_relative(root));
        }
    }

    if (!missing_headers.empty()) {
        std::cerr << "renamed public async headers should exist:\n";
        for (const auto& header : missing_headers) {
            std::cerr << "  - " << header.string() << '\n';
        }
        return 1;
    }

    if (!remaining_legacy_headers.empty()) {
        std::cerr << "legacy public async headers should be removed:\n";
        for (const auto& header : remaining_legacy_headers) {
            std::cerr << "  - " << header.string() << '\n';
        }
        return 1;
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
