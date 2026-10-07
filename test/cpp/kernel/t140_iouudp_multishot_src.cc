/**
 * @file t140_iouudp_multishot_src.cc
 * @brief 锁定 io_uring UDP multishot recvmsg、provided buffer 与 one-shot 回退源码边界。
 * @details 验证 UDP 使用独立的数据报 ready queue，完成路径解析 recvmsg 元数据和源地址，
 *          并保留能力探测失败时的 one-shot recvmsg 路径。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

bool require_text(const std::string& text, const std::string& needle, const char* message)
{
    if (text.find(needle) != std::string::npos) {
        return true;
    }
    std::cerr << "[T140] " << message << '\n';
    return false;
}

bool reject_text(const std::string& text, const std::string& needle, const char* message)
{
    if (text.find(needle) == std::string::npos) {
        return true;
    }
    std::cerr << "[T140] " << message << '\n';
    return false;
}

std::string extract_section(const std::string& content,
                           const std::string& begin,
                           const std::string& end)
{
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

int main()
{
    const std::filesystem::path root(GALAY_SOURCE_ROOT);
    const std::string controller = read_all(root / "galay-kernel/core/io_controller.hpp");
    const std::string& scheduler = controller;
    const std::string reactor_h = read_all(root / "galay-kernel/core/uring_reactor.h");
    const std::string reactor_cc = read_all(root / "galay-kernel/core/uring_reactor.cc");
    if (controller.empty() || scheduler.empty() || reactor_h.empty() || reactor_cc.empty()) {
        std::cerr << "[T140] failed to read io_uring source files\n";
        return 1;
    }

    bool ok = true;
    ok = require_text(controller, "ReadyRecvDatagram",
                     "expected a datagram-specific ready queue entry") && ok;
    ok = require_text(controller, "m_ready_recvfrom",
                     "expected IOController to queue complete UDP datagrams") && ok;
    ok = require_text(controller, "try_consume_ready_recv_from",
                     "expected queued datagrams to be delivered without stream concatenation") && ok;
    ok = require_text(controller, "m_recvfrom_multishot_armed",
                     "expected IOController to track persistent recvmsg state") && ok;
    ok = require_text(controller, "m_recvfrom_result_assigned",
                     "expected one suspended recvfrom awaitable to receive at most one datagram") && ok;

    ok = require_text(scheduler, "m_recvfrom_result_assigned = false",
                     "expected recvfrom awaitable rebinding to preserve the persistent request epoch") && ok;

    ok = require_text(reactor_h, "submit_multishot_recv_from",
                     "expected a dedicated UDP multishot submit path") && ok;
    ok = require_text(reactor_h, "process_recv_from_completion",
                     "expected a dedicated UDP multishot completion path") && ok;
    ok = require_text(reactor_h, "m_recvmsg_multishot_supported",
                     "expected runtime capability gating for recvmsg multishot") && ok;
    ok = require_text(reactor_h, "m_recvmsg_multishot_confirmed",
                     "expected successful CQEs to confirm recvmsg multishot capability") && ok;
    ok = require_text(reactor_h, "initialize_recv_from_buffer_pool",
                     "expected lazy UDP provided-buffer initialization") && ok;

    ok = require_text(reactor_cc, "io_uring_prep_recvmsg_multishot(",
                     "expected recvmsg multishot SQE preparation") && ok;
    ok = require_text(reactor_cc, "kernel_at_least(6, 0)",
                     "expected the UDP multishot path to require Linux kernel 6.0+") && ok;
    ok = require_text(reactor_cc, "io_uring_opcode_supported(probe, IORING_OP_RECVMSG)",
                     "expected RECVMSG opcode probing before multishot enablement") && ok;
    ok = require_text(reactor_cc, "io_uring_recvmsg_validate(",
                     "expected recvmsg CQE metadata validation") && ok;
    ok = require_text(reactor_cc, "io_uring_recvmsg_payload(",
                     "expected payload extraction from the selected buffer") && ok;
    ok = require_text(reactor_cc, "io_uring_recvmsg_name(",
                     "expected source address extraction from recvmsg output") && ok;
    ok = require_text(reactor_cc, "add_recv_from_one_shot",
                     "expected the existing one-shot recvmsg behavior to remain as fallback") && ok;
    ok = require_text(
             reactor_cc,
             "#if GALAY_HAS_IO_URING_RECVMSG_MULTISHOT\ninline bool kernel_at_least",
             "expected the kernel-version helper to be absent from old-liburing builds") && ok;

    const std::string start = extract_section(
        reactor_cc,
        "std::expected<void, IOError> IOUringReactor::start()",
        "IOUringReactor::~IOUringReactor()");
    ok = reject_text(start, "kRecvFromBufferCount",
                    "reactor start must not eagerly allocate the optional UDP pool") && ok;

    const std::string add_recvfrom = extract_section(
        reactor_cc,
        "int IOUringReactor::add_recv_from(IOController* controller)",
        "std::expected<void, IOError> IOUringReactor::initialize_recv_from_buffer_pool()");
    ok = require_text(add_recvfrom, "io_error_code_from_error(error)",
                     "expected lazy UDP pool failures to preserve the framework error") && ok;
    ok = require_text(add_recvfrom, "system_code_from_error(error)",
                     "expected lazy UDP pool failures to preserve the system error") && ok;

    const std::string completion = extract_section(
        reactor_cc,
        "void IOUringReactor::process_recv_from_completion",
        "}  // namespace galay::kernel");
    ok = require_text(completion, "cqe->res == -EINVAL && !m_recvmsg_multishot_confirmed",
                     "expected EINVAL to disable multishot only before capability confirmation") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "T140-IOUringUdpMultishotSourceCase PASS\n";
    return 0;
}
