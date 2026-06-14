#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef GALAY_MONGO_SOURCE_DIR
#define GALAY_MONGO_SOURCE_DIR "."
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
    const std::filesystem::path source_root = GALAY_MONGO_SOURCE_DIR;
    const auto socket_options_path = source_root / "galay-mongo" / "base" / "socket_options.h";
    const auto mongo_config = readFile(source_root / "galay-mongo" / "base" / "mongo_config.h");
    const auto connection = readFile(source_root / "galay-mongo" / "protocol" / "connection.cc");
    const auto connection_header = readFile(source_root / "galay-mongo" / "protocol" / "connection.h");
    const auto async_client = readFile(source_root / "galay-mongo" / "async" / "client.cc");

    if (std::filesystem::exists(socket_options_path)) {
        std::cerr << "local socket_options.h wrapper must not exist\n";
        return 1;
    }

    if (contains(mongo_config, "socket_timeout_ms") ||
        contains(mongo_config, "kDefaultSocketTimeoutMs") ||
        contains(connection_header, "socket_timeout_ms")) {
        std::cerr << "socket-level timeout config must not be exposed\n";
        return 1;
    }

    if (contains(connection, "trySetTcpNoDelay") ||
        contains(connection, "trySetSocketTimeoutMs") ||
        contains(connection, "SO_RCVTIMEO") ||
        contains(connection, "SO_SNDTIMEO")) {
        std::cerr << "sync connection must not use local socket option helpers or socket timeouts\n";
        return 1;
    }

    if (contains(async_client, "trySetTcpNoDelay") ||
        contains(async_client, "trySetSocketTimeoutMs") ||
        contains(async_client, "SO_RCVTIMEO") ||
        contains(async_client, "SO_SNDTIMEO")) {
        std::cerr << "async client must not use local socket option helpers or socket timeouts\n";
        return 1;
    }

    if (!contains(connection, "galay::kernel::HandleOption") ||
        !contains(connection, "::GHandle") ||
        !contains(connection, "handleTcpNoDelay()")) {
        std::cerr << "sync TCP_NODELAY must use galay-kernel HandleOption directly\n";
        return 1;
    }

    if (contains(connection, "(void)::galay::kernel::HandleOption")) {
        std::cerr << "sync TCP_NODELAY must handle HandleOption errors\n";
        return 1;
    }

    if (!contains(async_client, "client.m_socket.option().handleTcpNoDelay()")) {
        std::cerr << "async TCP_NODELAY must use TcpSocket HandleOption directly\n";
        return 1;
    }

    if (contains(async_client, "(void)client.m_socket.option().handleTcpNoDelay()")) {
        std::cerr << "async TCP_NODELAY must handle HandleOption errors\n";
        return 1;
    }

    std::cout << "T9-SocketOptionsSource PASS\n";
    return 0;
}
