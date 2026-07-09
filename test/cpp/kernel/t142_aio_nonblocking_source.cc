/**
 * @file t142_aio_nonblocking_source.cc
 * @brief 防止 epoll AIO 完成收割在 reactor 线程阻塞。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

int main()
{
    const std::filesystem::path source_root = GALAY_SOURCE_ROOT;
    const auto reactor_path = source_root / "galay-kernel" / "core" / "epoll_reactor.cc";
    const std::string content = readAll(reactor_path);
    if (content.empty()) {
        std::cerr << "failed to read " << reactor_path << "\n";
        return 1;
    }

    const auto call_pos = content.find("io_getevents(aio_awaitable->m_aio_ctx");
    if (call_pos == std::string::npos) {
        std::cerr << "AIO completion path no longer contains io_getevents; review this test\n";
        return 1;
    }

    const auto block_end = content.find(");", call_pos);
    if (block_end == std::string::npos) {
        std::cerr << "failed to locate io_getevents call end\n";
        return 1;
    }
    const std::string call = content.substr(call_pos, block_end - call_pos);

    if (call.find("\n                                                            1,") != std::string::npos ||
        call.find("nullptr") != std::string::npos) {
        std::cerr << "AIO completion harvest must use non-blocking io_getevents\n";
        return 1;
    }

    if (content.find("timespec timeout{0, 0}") == std::string::npos &&
        content.find("timespec timeout = {0, 0}") == std::string::npos) {
        std::cerr << "AIO completion path should use an explicit zero timeout\n";
        return 1;
    }

    return 0;
}
