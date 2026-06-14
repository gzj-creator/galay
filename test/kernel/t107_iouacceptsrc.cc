/**
 * @file t107_iouacceptsrc.cc
 * @brief 用途：锁定 io_uring multishot accept 的源码边界。
 * 关键覆盖点：multishot accept 提交、CQE `IORING_CQE_F_MORE`、controller 侧 ready queue。
 * 通过条件：源码包含 multishot accept token，且不再依赖 awaitable host 缓冲作为长期 accept 地址存储。
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

std::string extractFunction(const std::string& content,
                            const std::string& signature) {
    const auto begin_pos = content.find(signature);
    if (begin_pos == std::string::npos) {
        return {};
    }
    const auto body_begin = content.find('{', begin_pos);
    if (body_begin == std::string::npos) {
        return {};
    }

    int depth = 0;
    for (size_t index = body_begin; index < content.size(); ++index) {
        if (content[index] == '{') {
            ++depth;
        } else if (content[index] == '}') {
            --depth;
            if (depth == 0) {
                return content.substr(begin_pos, index - begin_pos + 1);
            }
        }
    }
    return {};
}

}  // namespace

int main() {
    const auto root = projectRoot();
    const auto iocontroller = root / "galay-kernel" / "kernel" / "io_controller.hpp";
    const auto iouring = root / "galay-kernel" / "kernel" / "uring_reactor.cc";

    const std::string iocontroller_text = readAll(iocontroller);
    const std::string iouring_text = readAll(iouring);
    if (iocontroller_text.empty() || iouring_text.empty()) {
        std::cerr << "[T107] failed to read source files\n";
        return 1;
    }

    if (!containsText(iocontroller_text, "std::deque<GHandle> m_ready_accepts")) {
        std::cerr << "[T107] expected IOController to keep queued accepted handles\n";
        return 1;
    }
    if (!containsText(iocontroller_text, "bool m_accept_multishot_armed")) {
        std::cerr << "[T107] expected IOController to track multishot accept armed state\n";
        return 1;
    }
    if (!containsText(iocontroller_text, "bool m_accept_result_assigned")) {
        std::cerr << "[T107] expected IOController to track active accept delivery state\n";
        return 1;
    }

    if (!containsText(iouring_text, "io_uring_prep_multishot_accept(")) {
        std::cerr << "[T107] expected IOUringReactor to use multishot accept submission\n";
        return 1;
    }
    if (!containsText(iouring_text, "IORING_CQE_F_MORE")) {
        std::cerr << "[T107] expected IOUringReactor accept path to inspect IORING_CQE_F_MORE\n";
        return 1;
    }
    if (!containsText(iouring_text, "getpeername(") &&
        !containsText(iocontroller_text, "getpeername(")) {
        std::cerr << "[T107] expected multishot accept delivery to resolve peer host lazily\n";
        return 1;
    }

    const std::string add_accept = extractFunction(
        iouring_text,
        "int IOUringReactor::addAccept(IOController* controller) {");
    if (add_accept.empty()) {
        std::cerr << "[T107] failed to isolate IOUringReactor::addAccept\n";
        return 1;
    }
    if (containsText(add_accept, "awaitable->m_host->sockAddr()")) {
        std::cerr << "[T107] expected addAccept to stop borrowing awaitable host buffer\n";
        return 1;
    }

    std::cout << "T107-IOUringMultishotAcceptSourceCase PASS\n";
    return 0;
}
