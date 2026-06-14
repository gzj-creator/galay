/**
 * @file t106_epolllazysrc.cc
 * @brief 用途：锁定 epoll lazy registration 的源码边界。
 * 关键覆盖点：pending queue、flush 接口、`applyEvents(...)` 不直接 `epoll_ctl`。
 * 通过条件：源码包含延迟注册 token，且 event loop 会在稳定时机 flush。
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
    const auto epoll_header = root / "galay-kernel" / "kernel" / "epoll_reactor.h";
    const auto epoll_source = root / "galay-kernel" / "kernel" / "epoll_reactor.cc";
    const auto epoll_scheduler = root / "galay-kernel" / "kernel" / "epoll_scheduler.cc";

    const std::string epoll_header_text = readAll(epoll_header);
    const std::string epoll_source_text = readAll(epoll_source);
    const std::string epoll_scheduler_text = readAll(epoll_scheduler);
    if (epoll_header_text.empty() || epoll_source_text.empty() || epoll_scheduler_text.empty()) {
        std::cerr << "[T106] failed to read epoll source files\n";
        return 1;
    }

    if (!containsText(epoll_header_text, "struct PendingChange")) {
        std::cerr << "[T106] expected EpollReactor to define PendingChange\n";
        return 1;
    }
    if (!containsText(epoll_header_text, "int flushPendingChanges()")) {
        std::cerr << "[T106] expected EpollReactor to expose flushPendingChanges()\n";
        return 1;
    }
    if (!containsText(epoll_header_text, "std::vector<PendingChange> m_pending_changes")) {
        std::cerr << "[T106] expected EpollReactor to keep pending change queue\n";
        return 1;
    }
    if (!containsText(epoll_header_text, "BATCH_THRESHOLD")) {
        std::cerr << "[T106] expected EpollReactor to keep a batch threshold\n";
        return 1;
    }

    const std::string apply_events = extractFunction(
        epoll_source_text,
        "int EpollReactor::applyEvents(IOController* controller, uint32_t events) {");
    if (apply_events.empty()) {
        std::cerr << "[T106] failed to isolate EpollReactor::applyEvents\n";
        return 1;
    }
    if (containsText(apply_events, "epoll_ctl(")) {
        std::cerr << "[T106] expected applyEvents to stop calling epoll_ctl directly\n";
        return 1;
    }
    if (!containsText(apply_events, "m_pending_changes.push_back")) {
        std::cerr << "[T106] expected applyEvents to enqueue pending changes\n";
        return 1;
    }

    if (!containsText(epoll_source_text, "int EpollReactor::flushPendingChanges()")) {
        std::cerr << "[T106] expected EpollReactor to implement flushPendingChanges()\n";
        return 1;
    }
    if (!containsText(epoll_source_text, "if (flushPendingChanges() < 0)")) {
        std::cerr << "[T106] expected poll() to flush pending changes before epoll_wait\n";
        return 1;
    }
    if (!containsText(epoll_source_text, "m_pending_changes.erase(")) {
        std::cerr << "[T106] expected flush path to erase completed pending changes\n";
        return 1;
    }

    if (!containsText(epoll_scheduler_text, "[this]() { (void)m_reactor.flushPendingChanges(); }")) {
        std::cerr << "[T106] expected EpollScheduler event loop to flush pending changes post-pass\n";
        return 1;
    }

    std::cout << "T106-EpollLazyRegistrationSourceCase PASS\n";
    return 0;
}
