/**
 * @file t110_iourecvsrc.cc
 * @brief 用途：锁定 io_uring multishot recv + provided buffer ring 的源码边界。
 * 关键覆盖点：provided buffer ring 建立、buffer select、multishot recv 提交、controller 侧 ready recv queue。
 * 通过条件：源码包含目标 token，并且 recv completion 路径开始解析 buffer id。
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
    const auto iocontroller = root / "galay-kernel" / "kernel" / "io_controller.hpp";
    const auto iouring_h = root / "galay-kernel" / "kernel" / "uring_reactor.h";
    const auto iouring_cc = root / "galay-kernel" / "kernel" / "uring_reactor.cc";

    const std::string iocontroller_text = readAll(iocontroller);
    const std::string iouring_h_text = readAll(iouring_h);
    const std::string iouring_cc_text = readAll(iouring_cc);
    if (iocontroller_text.empty() || iouring_h_text.empty() || iouring_cc_text.empty()) {
        std::cerr << "[T110] failed to read source files\n";
        return 1;
    }

    if (!containsText(iocontroller_text, "m_ready_recvs")) {
        std::cerr << "[T110] expected IOController to keep queued ready recv payloads\n";
        return 1;
    }
    if (!containsText(iocontroller_text, "m_recv_multishot_armed")) {
        std::cerr << "[T110] expected IOController to track multishot recv armed state\n";
        return 1;
    }
    if (!containsText(iocontroller_text, "m_recv_multishot_token")) {
        std::cerr << "[T110] expected IOController to track active multishot recv token\n";
        return 1;
    }

    if (!containsText(iouring_h_text, "processRecvCompletion")) {
        std::cerr << "[T110] expected IOUringReactor to define processRecvCompletion helper\n";
        return 1;
    }
    if (!containsText(iouring_cc_text, "io_uring_setup_buf_ring(")) {
        std::cerr << "[T110] expected IOUringReactor to allocate a provided buffer ring\n";
        return 1;
    }
    if (!containsText(iouring_cc_text, "io_uring_prep_recv_multishot(")) {
        std::cerr << "[T110] expected IOUringReactor recv path to use multishot recv submission\n";
        return 1;
    }
    if (!containsText(iouring_cc_text, "IOSQE_BUFFER_SELECT")) {
        std::cerr << "[T110] expected IOUringReactor recv SQE to enable IOSQE_BUFFER_SELECT\n";
        return 1;
    }
    if (!containsText(iouring_cc_text, "IORING_CQE_F_BUFFER")) {
        std::cerr << "[T110] expected recv completion path to inspect IORING_CQE_F_BUFFER\n";
        return 1;
    }
    if (!containsText(iouring_cc_text, "io_uring_buf_ring_add(") ||
        !containsText(iouring_cc_text, "io_uring_buf_ring_advance(")) {
        std::cerr << "[T110] expected IOUringReactor to return consumed buffers back to buf ring\n";
        return 1;
    }

    std::cout << "T110-IOUringMultishotRecvSourceCase PASS\n";
    return 0;
}
