/**
 * @file t149_epoll_persistent_read_source.cc
 * @brief 锁定 epoll RECV/READV 持久 READ 兴趣与关闭清理的源码边界。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string readAll(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
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

bool contains(const std::string& content, const std::string& token) {
    return content.find(token) != std::string::npos;
}

bool expectToken(const std::string& content, const std::string& token, const char* label) {
    if (contains(content, token)) {
        return true;
    }
    std::cerr << "[T149] " << label << " missing token: " << token << '\n';
    return false;
}

}  // namespace

int main() {
    const auto root = std::filesystem::path(GALAY_SOURCE_ROOT) / "galay-kernel" / "core";
    const std::string controller = readAll(root / "io_controller.hpp");
    const std::string reactor = readAll(root / "epoll_reactor.cc");
    if (controller.empty() || reactor.empty()) {
        std::cerr << "[T149] failed to read epoll sources\n";
        return 1;
    }

    bool passed = true;
    passed = expectToken(controller,
                         "uint32_t m_persistent_events = 0;",
                         "IOController") && passed;

    const std::string build_events = extractSection(
        reactor,
        "uint32_t EpollReactor::buildEvents",
        "int EpollReactor::applyEvents");
    passed = expectToken(build_events,
                         "controller->m_persistent_events",
                         "buildEvents") && passed;

    const std::string add_recv = extractSection(
        reactor,
        "int EpollReactor::addRecv(IOController* controller)",
        "int EpollReactor::addSend(IOController* controller)");
    passed = expectToken(add_recv, "armPersistentRead(controller)", "addRecv") && passed;

    const std::string add_readv = extractSection(
        reactor,
        "int EpollReactor::addReadv(IOController* controller)",
        "int EpollReactor::addWritev(IOController* controller)");
    passed = expectToken(add_readv, "armPersistentRead(controller)", "addReadv") && passed;

    const std::string add_sequence = extractSection(
        reactor,
        "int EpollReactor::addSequence(IOController* controller)",
        "int EpollReactor::remove(IOController* controller)");
    passed = expectToken(add_sequence,
                         "return armPersistentRead(controller);",
                         "addSequence") && passed;

    const std::string completion = extractSection(
        reactor,
        "const auto complete_one_shot",
        "if (ev.events & EPOLLIN)");
    passed = expectToken(completion,
                         "controller->removeAwaitable(event_type);",
                         "completion") && passed;
    if (contains(completion, "controller->m_persistent_events = 0")) {
        std::cerr << "[T149] completion path must not disarm persistent READ\n";
        passed = false;
    }

    const std::string remove = extractSection(
        reactor,
        "int EpollReactor::remove(IOController* controller)",
        "int EpollReactor::processSequence");
    passed = expectToken(remove,
                         "controller->m_persistent_events = 0;",
                         "remove") && passed;

    const std::string close = extractSection(
        reactor,
        "int EpollReactor::addClose(IOController* controller)",
        "int EpollReactor::addFileRead(IOController* controller)");
    passed = expectToken(close,
                         "controller->m_persistent_events = 0;",
                         "addClose") && passed;

    if (!passed) {
        return 1;
    }
    std::cout << "T149-EpollPersistentReadSource PASS\n";
    return 0;
}
