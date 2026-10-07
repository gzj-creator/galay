/**
 * @file t149_epoll_persistent_read_source.cc
 * @brief 锁定 epoll RECV/READV 持久 READ 兴趣与关闭清理的源码边界。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

std::string extract_section(const std::string& content,
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

bool contains(const std::string& content, const std::string& token) {
    return content.find(token) != std::string::npos;
}

bool expect_token(const std::string& content, const std::string& token, const char* label) {
    if (contains(content, token)) {
        return true;
    }
    std::cerr << "[T149] " << label << " missing token: " << token << '\n';
    return false;
}

}  // namespace

int main() {
    const auto root = std::filesystem::path(GALAY_SOURCE_ROOT) / "galay-kernel" / "core";
    const std::string controller = read_all(root / "io_controller.hpp");
    const std::string reactor = read_all(root / "epoll_reactor.cc");
    if (controller.empty() || reactor.empty()) {
        std::cerr << "[T149] failed to read epoll sources\n";
        return 1;
    }

    bool passed = true;
    passed = expect_token(controller,
                         "uint32_t m_persistent_events = 0;",
                         "IOController") && passed;

    const std::string build_events = extract_section(
        reactor,
        "uint32_t EpollReactor::build_events",
        "int EpollReactor::apply_events");
    passed = expect_token(build_events,
                         "controller->m_persistent_events",
                         "build_events") && passed;

    const std::string add_recv = extract_section(
        reactor,
        "int EpollReactor::add_recv(IOController* controller)",
        "int EpollReactor::add_send(IOController* controller)");
    passed = expect_token(add_recv, "arm_persistent_read(controller)", "add_recv") && passed;

    const std::string add_readv = extract_section(
        reactor,
        "int EpollReactor::add_readv(IOController* controller)",
        "int EpollReactor::add_writev(IOController* controller)");
    passed = expect_token(add_readv, "arm_persistent_read(controller)", "add_readv") && passed;

    const std::string add_sequence = extract_section(
        reactor,
        "int EpollReactor::add_sequence(IOController* controller)",
        "int EpollReactor::remove(IOController* controller)");
    passed = expect_token(add_sequence,
                         "return arm_persistent_read(controller);",
                         "add_sequence") && passed;

    const std::string completion = extract_section(
        reactor,
        "const auto complete_one_shot",
        "if (ev.events & EPOLLIN)");
    passed = expect_token(completion,
                         "controller->remove_awaitable(event_type);",
                         "completion") && passed;
    if (contains(completion, "controller->m_persistent_events = 0")) {
        std::cerr << "[T149] completion path must not disarm persistent READ\n";
        passed = false;
    }

    const std::string remove = extract_section(
        reactor,
        "int EpollReactor::remove(IOController* controller)",
        "int EpollReactor::process_sequence");
    passed = expect_token(remove,
                         "controller->m_persistent_events = 0;",
                         "remove") && passed;

    const std::string close = extract_section(
        reactor,
        "int EpollReactor::add_close(IOController* controller)",
        "int EpollReactor::add_file_read(IOController* controller)");
    passed = expect_token(close,
                         "controller->m_persistent_events = 0;",
                         "add_close") && passed;

    if (!passed) {
        return 1;
    }
    std::cout << "T149-EpollPersistentReadSource PASS\n";
    return 0;
}
