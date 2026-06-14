/**
 * @file t93_kqseqregsrc.cc
 * @brief 用途：锁定 kqueue sequence 路径的持久注册实现，避免退回事件后重挂。
 * 关键覆盖点：`applySequenceInterest(...)` 使用 `EV_CLEAR` 持久订阅；`processEvent(...)`
 * 不再在 sequence 命中后清掉 `m_sequence_armed_mask`。
 * 通过条件：源码包含持久注册 token，且不包含旧的命中后清 arm 逻辑。
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

std::string extractSection(const std::string& content,
                           const std::string& begin,
                           const std::string& end) {
    const auto begin_pos = content.find(begin);
    if (begin_pos == std::string::npos) {
        return {};
    }
    const auto end_pos = content.find(end, begin_pos);
    if (end_pos == std::string::npos || end_pos <= begin_pos) {
        return content.substr(begin_pos);
    }
    return content.substr(begin_pos, end_pos - begin_pos);
}

}  // namespace

int main() {
    const auto source =
        projectRoot() / "galay-kernel" / "kernel" / "kqueue_reactor.cc";
    const std::string content = readAll(source);
    if (content.empty()) {
        std::cerr << "[T93] failed to read source file: " << source << '\n';
        return 1;
    }

    if (!containsText(content, "append_change(IOController::READ, EV_ADD | EV_CLEAR);")) {
        std::cerr << "[T93] expected read-side sequence registration to use EV_CLEAR\n";
        return 1;
    }

    if (!containsText(content, "append_change(IOController::WRITE, EV_ADD | EV_CLEAR);")) {
        std::cerr << "[T93] expected write-side sequence registration to use EV_CLEAR\n";
        return 1;
    }

    const std::string add_accept = extractSection(
        content,
        "int KqueueReactor::addAccept(IOController* controller) {",
        "int KqueueReactor::addConnect(IOController* controller) {");
    if (add_accept.empty()) {
        std::cerr << "[T93] failed to isolate KqueueReactor::addAccept implementation\n";
        return 1;
    }
    if (!containsText(add_accept, "return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);")) {
        std::cerr << "[T93] expected addAccept to register immediately via kevent\n";
        return 1;
    }
    if (containsText(add_accept, "m_pending_changes.push_back(ev);")) {
        std::cerr << "[T93] expected addAccept to avoid delayed pending_changes registration\n";
        return 1;
    }

    if (containsText(content,
                     "controller->m_sequence_armed_mask =\n"
                     "            static_cast<uint8_t>(controller->m_sequence_armed_mask & ~fired_mask);")) {
        std::cerr << "[T93] expected sequence event path to keep persistent armed mask\n";
        return 1;
    }

    std::cout << "T93-KqueueSequencePersistentRegistrationSourceCase PASS\n";
    return 0;
}
