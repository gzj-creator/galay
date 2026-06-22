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
    const auto pool_header = readFile(root / "src/cpp/galay-redis/async/conn_pool.h");
    const auto pool_source = readFile(root / "src/cpp/galay-redis/async/conn_pool.cc");
    const auto session_source = readFile(root / "src/cpp/galay-redis/sync/redis_session.cc");

    for (const auto* forbidden : {
             "m_pool->initializeSync()",
             "m_pool->acquireSync(",
             "std::condition_variable",
             "暂时创建未连接的客户端",
             "No available connections",
         }) {
        if (contains(pool_header, forbidden) || contains(pool_source, forbidden)) {
            std::cerr << "redis pool must not use old blocking/unconnected placeholder path: "
                      << forbidden << "\n";
            return 1;
        }
    }

    if (contains(session_source, "return std::unexpected(select_reply.error())")) {
        std::cerr << "selectDB unexpected reply shape must not read expected::error() on value path\n";
        return 1;
    }

    std::cout << "T22-RedisPoolSourceBoundaries PASS\n";
    return 0;
}
