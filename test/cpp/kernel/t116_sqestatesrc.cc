/**
 * @file t116_sqestatesrc.cc
 * @brief 用途：锁定 IOController 中 SqeState 所有权收口后的源码边界。
 * 关键覆盖点：m_sqe_state 不再使用 shared_ptr，SqeRequestHandle 与 IOController
 * 仅借用 handle arena 内部的稳定 SqeState 地址，handle arena 继续用 shared_ptr 自保活。
 * 通过条件：源码包含目标 handle，且移除了 SqeState 的 shared_ptr 所有权层。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::filesystem::path projectRoot() {
    return std::filesystem::path(GALAY_SOURCE_ROOT);
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
    const auto controller_h = root / "galay-kernel" / "core" / "io_controller.hpp";
    const std::string text = readAll(controller_h);
    if (text.empty()) {
        std::cerr << "[T116] failed to read io_controller.hpp\n";
        return 1;
    }

    if (containsText(text, "std::shared_ptr<SqeState> m_sqe_state[SIZE]")) {
        std::cerr << "[T116] expected IOController m_sqe_state to stop using shared_ptr\n";
        return 1;
    }
    if (!containsText(text, "SqeState* state = nullptr;")) {
        std::cerr << "[T116] expected SqeRequestHandle to borrow SqeState via raw pointer\n";
        return 1;
    }
    if (!containsText(text, "SqeState m_state;")) {
        std::cerr << "[T116] expected SqeHandleArena to embed a stable SqeState\n";
        return 1;
    }
    if (!containsText(text, "SqeState* m_sqe_state[SIZE] = {nullptr, nullptr};")) {
        std::cerr << "[T116] expected IOController to borrow SqeState via raw pointers\n";
        return 1;
    }
    if (!containsText(text, "std::shared_ptr<SqeHandleArena> m_sqe_handle_pool[SIZE]")) {
        std::cerr << "[T116] expected handle arena shared ownership to remain for late CQE safety\n";
        return 1;
    }

    std::cout << "T116-IOControllerSqeStateSourceCase PASS\n";
    return 0;
}
