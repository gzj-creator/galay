#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef GALAY_MYSQL_SOURCE_DIR
#define GALAY_MYSQL_SOURCE_DIR "."
#endif

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

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    const std::filesystem::path source_root = GALAY_MYSQL_SOURCE_DIR;
    const auto pool_header = readFile(source_root / "galay-mysql" / "async" / "conn_pool.h");
    const auto pool_source = readFile(source_root / "galay-mysql" / "async" / "conn_pool.cc");

    for (const auto* forbidden : {
             "std::mutex",
             "std::condition_variable",
             "std::lock_guard",
             "std::unique_lock",
             ".wait(",
         }) {
        if (contains(pool_header, forbidden) || contains(pool_source, forbidden)) {
            std::cerr << "async MySQL pool coroutine path must not use blocking synchronization: "
                      << forbidden << "\n";
            return 1;
        }
    }

    std::cout << "T13-MySQLPoolCoroutineSource PASS\n";
    return 0;
}
