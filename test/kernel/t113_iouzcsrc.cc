/**
 * @file t113_iouzcsrc.cc
 * @brief 用途：锁定 io_uring send_zc 的源码边界。
 * 关键覆盖点：能力探测、send_zc 提交、notification CQE 处理、零拷贝发送阈值门控。
 * 通过条件：源码包含目标 token，且 send 路径不再只有普通 prep_send。
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
    const auto controller = root / "galay-kernel" / "kernel" / "io_controller.hpp";
    const auto reactor_h = root / "galay-kernel" / "kernel" / "uring_reactor.h";
    const auto reactor_cc = root / "galay-kernel" / "kernel" / "uring_reactor.cc";

    const std::string controller_text = readAll(controller);
    const std::string reactor_h_text = readAll(reactor_h);
    const std::string reactor_cc_text = readAll(reactor_cc);
    if (controller_text.empty() || reactor_h_text.empty() || reactor_cc_text.empty()) {
        std::cerr << "[T113] failed to read source files\n";
        return 1;
    }

    if (!containsText(controller_text, "notify_expected = false;")) {
        std::cerr << "[T113] expected SqeRequestToken to track zero-copy notification state\n";
        return 1;
    }
    if (!containsText(reactor_h_text, "kSendZcThreshold")) {
        std::cerr << "[T113] expected IOUringReactor to define a send_zc size threshold\n";
        return 1;
    }
    if (!containsText(reactor_h_text, "m_send_zc_supported")) {
        std::cerr << "[T113] expected IOUringReactor to cache send_zc capability\n";
        return 1;
    }
    if (!containsText(reactor_cc_text, "io_uring_get_probe_ring(") ||
        !containsText(reactor_cc_text, "io_uring_opcode_supported(")) {
        std::cerr << "[T113] expected IOUringReactor to probe send_zc opcode support\n";
        return 1;
    }
    if (!containsText(reactor_cc_text, "io_uring_prep_send_zc(")) {
        std::cerr << "[T113] expected IOUringReactor send path to submit io_uring_prep_send_zc\n";
        return 1;
    }
    if (!containsText(reactor_cc_text, "IORING_SEND_ZC_REPORT_USAGE")) {
        std::cerr << "[T113] expected send_zc submissions to request usage reporting\n";
        return 1;
    }
    if (!containsText(reactor_cc_text, "IORING_CQE_F_NOTIF")) {
        std::cerr << "[T113] expected completion path to filter notification CQEs\n";
        return 1;
    }

    std::cout << "T113-IOUringSendZcSourceCase PASS\n";
    return 0;
}
