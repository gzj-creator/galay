/**
 * @file t147_kqueue_registration_batch_source.cc
 * @brief 锁定 kqueue 简单槽位 armed 去抖与 pending batch 提交实现。
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

bool expect_section(const std::string& content,
                   const std::string& begin,
                   const std::string& end,
                   const std::string& expected,
                   const char* label) {
    const std::string section = extract_section(content, begin, end);
    if (section.empty() || !contains(section, expected)) {
        std::cerr << "[T147] " << label << " missing token: " << expected << '\n';
        return false;
    }
    if (contains(section, "return kevent(m_kqueue_fd, &ev, 1, nullptr, 0, nullptr);")) {
        std::cerr << "[T147] " << label << " still submits a direct kevent\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const auto root = std::filesystem::path(GALAY_SOURCE_ROOT) / "galay-kernel" / "core";
    const std::string controller = read_all(root / "io_controller.hpp");
    const std::string reactor = read_all(root / "kqueue_reactor.cc");
    if (controller.empty() || reactor.empty()) {
        std::cerr << "[T147] failed to read kqueue sources\n";
        return 1;
    }

    bool passed = true;
    if (!contains(controller, "uint8_t m_simple_armed_mask = 0;")) {
        std::cerr << "[T147] IOController lacks simple armed mask\n";
        passed = false;
    }
    passed = expect_section(
                 reactor,
                 "int KqueueReactor::add_recv(IOController* controller) {",
                 "int KqueueReactor::add_send(IOController* controller) {",
                 "update_simple_interest(controller, IOController::READ, true)",
                 "add_recv") && passed;
    passed = expect_section(
                 reactor,
                 "int KqueueReactor::add_send(IOController* controller) {",
                 "int KqueueReactor::add_readv(IOController* controller) {",
                 "update_simple_interest(controller, IOController::WRITE, true)",
                 "add_send") && passed;
    passed = expect_section(
                 reactor,
                 "int KqueueReactor::add_readv(IOController* controller) {",
                 "int KqueueReactor::add_writev(IOController* controller) {",
                 "update_simple_interest(controller, IOController::READ, true)",
                 "add_readv") && passed;
    passed = expect_section(
                 reactor,
                 "int KqueueReactor::add_writev(IOController* controller) {",
                 "int KqueueReactor::add_close(IOController* controller) {",
                 "update_simple_interest(controller, IOController::WRITE, true)",
                 "add_writev") && passed;

    const std::string update = extract_section(
        reactor,
        "int KqueueReactor::update_simple_interest",
        "int KqueueReactor::add_accept");
    if (!contains(update, "m_pending_changes.push_back") ||
        !contains(update, "BATCH_THRESHOLD")) {
        std::cerr << "[T147] simple interest changes are not buffered with threshold flush\n";
        passed = false;
    }

    const std::string completion = extract_section(
        reactor,
        "const auto complete_one_shot",
        "if (ev.filter == EVFILT_READ)");
    if (!contains(completion, "event_type == RECV || event_type == READV") ||
        !contains(completion, "update_simple_interest(controller, slot, false)")) {
        std::cerr << "[T147] completion path does not preserve READ and disarm WRITE\n";
        passed = false;
    }

    if (!passed) {
        return 1;
    }
    std::cout << "T147-KqueueRegistrationBatchSource PASS\n";
    return 0;
}
