/**
 * @file t112_rportsrc.cc
 * @brief 用途：锁定 benchmark server 侧的 SO_REUSEPORT 多 acceptor 结构。
 * 关键覆盖点：plain/iov benchmark server 按 scheduler 数创建多个 listener，并为每个 listener
 * 单独调度 acceptLoop；默认并行度受 benchmark helper 限制，避免压满机器。
 * 通过条件：源码包含目标 token，且不再只保留单 listener/单 scheduler accept 路径。
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

bool verifyServer(const std::filesystem::path& path, const std::string& text) {
    if (!containsText(text, "defaultBenchmarkSchedulerCount")) {
        std::cerr << "[" << path.filename().string()
                  << "] expected scheduler count to be capped by benchmark helper\n";
        return false;
    }
    if (!containsText(text, "std::vector<std::unique_ptr<IOSchedulerType>> schedulers;")) {
        std::cerr << "[" << path.filename().string()
                  << "] expected multiple schedulers for reuseport acceptors\n";
        return false;
    }
    if (!containsText(text, "std::vector<TcpSocket> listeners;")) {
        std::cerr << "[" << path.filename().string()
                  << "] expected one listener per scheduler\n";
        return false;
    }
    if (!containsText(text, "listener.option().handleReusePort()")) {
        std::cerr << "[" << path.filename().string()
                  << "] expected listener to enable SO_REUSEPORT\n";
        return false;
    }
    if (!containsText(text, "scheduleTask(*schedulers[i], acceptLoop(schedulers[i].get(), &listeners[i]))")) {
        std::cerr << "[" << path.filename().string()
                  << "] expected acceptLoop to be scheduled per scheduler/listener pair\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto b2 = root / "benchmark" / "b2_tcp.cc";
    const auto b11 = root / "benchmark" / "b11_iov.cc";
    const std::string b2_text = readAll(b2);
    const std::string b11_text = readAll(b11);
    if (b2_text.empty() || b11_text.empty()) {
        std::cerr << "[T112] failed to read benchmark server sources\n";
        return 1;
    }

    if (!verifyServer(b2, b2_text) || !verifyServer(b11, b11_text)) {
        return 1;
    }

    std::cout << "T112-RuntimeReuseportSourceCase PASS\n";
    return 0;
}
