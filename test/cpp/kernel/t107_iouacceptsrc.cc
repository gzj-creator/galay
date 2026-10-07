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

std::filesystem::path project_root() {
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

std::string extract_function(const std::string& content,
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
    const auto root = project_root();
    const auto iocontroller = root / "galay-kernel" / "core" / "io_controller.hpp";
    const auto iouring = root / "galay-kernel" / "core" / "uring_reactor.cc";

    const std::string iocontroller_text = read_all(iocontroller);
    const std::string iouring_text = read_all(iouring);
    if (iocontroller_text.empty() || iouring_text.empty()) {
        std::cerr << "[T107] failed to read source files\n";
        return 1;
    }

    if (!contains_text(iocontroller_text, "std::deque<GHandle> m_ready_accepts")) {
        std::cerr << "[T107] expected IOController to keep queued accepted handles\n";
        return 1;
    }
    if (!contains_text(iocontroller_text, "bool m_accept_multishot_armed")) {
        std::cerr << "[T107] expected IOController to track multishot accept armed state\n";
        return 1;
    }
    if (contains_text(iocontroller_text, "m_accept_result_assigned") ||
        !contains_text(iouring_text, "IOUringReactor::detach_accept") ||
        !contains_text(iouring_text, "awaitable->select_ready(*result)")) {
        std::cerr << "[T107] expected unique typed completion and detached frame entry\n";
        return 1;
    }

    if (!contains_text(iouring_text, "io_uring_prep_multishot_accept(")) {
        std::cerr << "[T107] expected IOUringReactor to use multishot accept submission\n";
        return 1;
    }
    if (!contains_text(iocontroller_text, "IOEventType multishot_type") ||
        !contains_text(iocontroller_text, "handle->multishot_type = IOEventType::INVALID") ||
        !contains_text(iouring_text, "handle->multishot_type = ACCEPT") ||
        !contains_text(iouring_text, "handle->multishot_type = RECV;") ||
        !contains_text(iouring_text, "handle->multishot_type = RECVFROM;") ||
        !contains_text(iouring_text, "handle->multishot_type == ACCEPT && cqe->res >= 0")) {
        std::cerr << "[T107] expected persistent result identity and stale accept fd cleanup\n";
        return 1;
    }
    if (!contains_text(iouring_text, "IORING_CQE_F_MORE")) {
        std::cerr << "[T107] expected IOUringReactor accept path to inspect IORING_CQE_F_MORE\n";
        return 1;
    }
    const auto awaitable_text = read_all(root / "galay-kernel" / "core" / "awaitable.cc");
    const auto select_ready = extract_function(awaitable_text, "bool AcceptAwaitable::select_ready(GHandle handle)");
    if (!contains_text(select_ready, "getpeername(") ||
        !contains_text(select_ready, "AcceptedConnection(handle, std::move(peer))")) {
        std::cerr << "[T107] expected multishot accept delivery to resolve peer host lazily\n";
        return 1;
    }
    if (!contains_text(iouring_text, "void IOUringReactor::stop_accepts()") ||
        !contains_text(iouring_text, "m_accept_registrations") ||
        !contains_text(iouring_text, "registration.handle->recycle()")) {
        std::cerr << "[T107] expected owner stop to retain and teardown accept registrations\n";
        return 1;
    }

    const std::string add_accept = extract_function(
        iouring_text,
        "int IOUringReactor::add_accept(IOController* controller) {");
    if (add_accept.empty()) {
        std::cerr << "[T107] failed to isolate IOUringReactor::add_accept\n";
        return 1;
    }
    if (contains_text(add_accept, "awaitable->m_host->sock_addr()")) {
        std::cerr << "[T107] expected add_accept to stop borrowing awaitable host buffer\n";
        return 1;
    }

    std::cout << "T107-IOUringMultishotAcceptSourceCase PASS\n";
    return 0;
}
