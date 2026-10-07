/**
 * @file t109_tcpdefersrc.cc
 * @brief 用途：锁定 TCP_DEFER_ACCEPT 选项与 benchmark 服务端接入点。
 * 关键覆盖点：HandleOption 暴露 handle_tcp_defer_accept、Linux 实现使用 TCP_DEFER_ACCEPT、TCP benchmark server 在 listen 前启用该选项。
 * 通过条件：源码包含目标 API、setsockopt 实现与 benchmark 调用点。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::filesystem::path project_root() {
    return std::filesystem::path(GALAY_PROJECT_ROOT);
}

std::filesystem::path source_root() {
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

bool contains_text(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const auto root = project_root();
    const auto source_root = ::source_root();
    const auto handle_option_h = source_root / "galay-kernel" / "common" / "handle_option.h";
    const auto handle_option_cc = source_root / "galay-kernel" / "common" / "handle_option.cc";
    const auto b2_server = root / "benchmark" / "cpp" / "kernel" / "b2_tcp_server_throughput.cc";
    const auto b11_server = root / "benchmark" / "cpp" / "kernel" / "b11_tcp_iov_server_throughput.cc";

    const std::string header_text = read_all(handle_option_h);
    const std::string source_text = read_all(handle_option_cc);
    const std::string b2_text = read_all(b2_server);
    const std::string b11_text = read_all(b11_server);

    if (header_text.empty() || source_text.empty() || b2_text.empty() || b11_text.empty()) {
        std::cerr << "[T109] failed to read source files\n";
        return 1;
    }

    if (!contains_text(header_text, "handle_tcp_defer_accept")) {
        std::cerr << "[T109] expected HandleOption to expose handle_tcp_defer_accept\n";
        return 1;
    }
    if (!contains_text(source_text, "TCP_DEFER_ACCEPT")) {
        std::cerr << "[T109] expected HandleOption implementation to use TCP_DEFER_ACCEPT\n";
        return 1;
    }
    if (!contains_text(source_text, "setsockopt(m_handle.fd, IPPROTO_TCP, TCP_DEFER_ACCEPT")) {
        std::cerr << "[T109] expected HandleOption implementation to call setsockopt for TCP_DEFER_ACCEPT\n";
        return 1;
    }
    if (!contains_text(b2_text, "handle_tcp_defer_accept()")) {
        std::cerr << "[T109] expected B2 TCP benchmark server to enable handle_tcp_defer_accept\n";
        return 1;
    }
    if (!contains_text(b11_text, "handle_tcp_defer_accept()")) {
        std::cerr << "[T109] expected B11 TCP IOV benchmark server to enable handle_tcp_defer_accept\n";
        return 1;
    }

    std::cout << "T109-TcpDeferAcceptSourceCase PASS\n";
    return 0;
}
