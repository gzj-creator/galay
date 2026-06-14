/**
 * @file t109_tcpdefersrc.cc
 * @brief 用途：锁定 TCP_DEFER_ACCEPT 选项与 benchmark 服务端接入点。
 * 关键覆盖点：HandleOption 暴露 handleTcpDeferAccept、Linux 实现使用 TCP_DEFER_ACCEPT、TCP benchmark server 在 listen 前启用该选项。
 * 通过条件：源码包含目标 API、setsockopt 实现与 benchmark 调用点。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
}

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

bool containsText(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto handle_option_h = root / "galay-kernel" / "common" / "handle_option.h";
    const auto handle_option_cc = root / "galay-kernel" / "common" / "handle_option.cc";
    const auto b2_server = root / "benchmark" / "b2_tcp.cc";
    const auto b11_server = root / "benchmark" / "b11_iov.cc";

    const std::string header_text = readAll(handle_option_h);
    const std::string source_text = readAll(handle_option_cc);
    const std::string b2_text = readAll(b2_server);
    const std::string b11_text = readAll(b11_server);

    if (header_text.empty() || source_text.empty() || b2_text.empty() || b11_text.empty()) {
        std::cerr << "[T109] failed to read source files\n";
        return 1;
    }

    if (!containsText(header_text, "handleTcpDeferAccept")) {
        std::cerr << "[T109] expected HandleOption to expose handleTcpDeferAccept\n";
        return 1;
    }
    if (!containsText(source_text, "TCP_DEFER_ACCEPT")) {
        std::cerr << "[T109] expected HandleOption implementation to use TCP_DEFER_ACCEPT\n";
        return 1;
    }
    if (!containsText(source_text, "setsockopt(m_handle.fd, IPPROTO_TCP, TCP_DEFER_ACCEPT")) {
        std::cerr << "[T109] expected HandleOption implementation to call setsockopt for TCP_DEFER_ACCEPT\n";
        return 1;
    }
    if (!containsText(b2_text, "listener.option().handleTcpDeferAccept();")) {
        std::cerr << "[T109] expected B2 TCP benchmark server to enable handleTcpDeferAccept\n";
        return 1;
    }
    if (!containsText(b11_text, "listener.option().handleTcpDeferAccept();")) {
        std::cerr << "[T109] expected B11 TCP IOV benchmark server to enable handleTcpDeferAccept\n";
        return 1;
    }

    std::cout << "T109-TcpDeferAcceptSourceCase PASS\n";
    return 0;
}
