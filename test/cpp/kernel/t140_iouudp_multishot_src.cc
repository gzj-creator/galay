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

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

bool requireText(const std::string& text, const std::string& needle, const char* message)
{
    if (text.find(needle) != std::string::npos) {
        return true;
    }
    std::cerr << "[T140] " << message << '\n';
    return false;
}

bool rejectText(const std::string& text, const std::string& needle, const char* message)
{
    if (text.find(needle) == std::string::npos) {
        return true;
    }
    std::cerr << "[T140] " << message << '\n';
    return false;
}

std::string extractSection(const std::string& content,
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
    const std::string controller = readAll(root / "galay-kernel/core/io_controller.hpp");
    const std::string scheduler = readAll(root / "galay-kernel/core/io_scheduler.hpp");
    const std::string reactor_h = readAll(root / "galay-kernel/core/uring_reactor.h");
    const std::string reactor_cc = readAll(root / "galay-kernel/core/uring_reactor.cc");
    if (controller.empty() || scheduler.empty() || reactor_h.empty() || reactor_cc.empty()) {
        std::cerr << "[T140] failed to read io_uring source files\n";
        return 1;
    }

    bool ok = true;
    ok = requireText(controller, "ReadyRecvDatagram",
                     "expected a datagram-specific ready queue entry") && ok;
    ok = requireText(controller, "m_ready_recvfrom",
                     "expected IOController to queue complete UDP datagrams") && ok;
    ok = requireText(controller, "tryConsumeReadyRecvFrom",
                     "expected queued datagrams to be delivered without stream concatenation") && ok;
    ok = requireText(controller, "m_recvfrom_multishot_armed",
                     "expected IOController to track persistent recvmsg state") && ok;
    ok = requireText(controller, "m_recvfrom_result_assigned",
                     "expected one suspended recvfrom awaitable to receive at most one datagram") && ok;

    ok = requireText(scheduler, "m_recvfrom_result_assigned = false",
                     "expected recvfrom awaitable rebinding to preserve the persistent request epoch") && ok;

    ok = requireText(reactor_h, "submitMultishotRecvFrom",
                     "expected a dedicated UDP multishot submit path") && ok;
    ok = requireText(reactor_h, "processRecvFromCompletion",
                     "expected a dedicated UDP multishot completion path") && ok;
    ok = requireText(reactor_h, "m_recvmsg_multishot_supported",
                     "expected runtime capability gating for recvmsg multishot") && ok;
    ok = requireText(reactor_h, "m_recvmsg_multishot_confirmed",
                     "expected successful CQEs to confirm recvmsg multishot capability") && ok;
    ok = requireText(reactor_h, "initializeRecvFromBufferPool",
                     "expected lazy UDP provided-buffer initialization") && ok;

    ok = requireText(reactor_cc, "io_uring_prep_recvmsg_multishot(",
                     "expected recvmsg multishot SQE preparation") && ok;
    ok = requireText(reactor_cc, "kernelAtLeast(6, 0)",
                     "expected the UDP multishot path to require Linux kernel 6.0+") && ok;
    ok = requireText(reactor_cc, "io_uring_opcode_supported(probe, IORING_OP_RECVMSG)",
                     "expected RECVMSG opcode probing before multishot enablement") && ok;
    ok = requireText(reactor_cc, "io_uring_recvmsg_validate(",
                     "expected recvmsg CQE metadata validation") && ok;
    ok = requireText(reactor_cc, "io_uring_recvmsg_payload(",
                     "expected payload extraction from the selected buffer") && ok;
    ok = requireText(reactor_cc, "io_uring_recvmsg_name(",
                     "expected source address extraction from recvmsg output") && ok;
    ok = requireText(reactor_cc, "addRecvFromOneShot",
                     "expected the existing one-shot recvmsg behavior to remain as fallback") && ok;
    ok = requireText(
             reactor_cc,
             "#if GALAY_HAS_IO_URING_RECVMSG_MULTISHOT\ninline bool kernelAtLeast",
             "expected the kernel-version helper to be absent from old-liburing builds") && ok;

    const std::string start = extractSection(
        reactor_cc,
        "std::expected<void, IOError> IOUringReactor::start()",
        "IOUringReactor::~IOUringReactor()");
    ok = rejectText(start, "kRecvFromBufferCount",
                    "reactor start must not eagerly allocate the optional UDP pool") && ok;

    const std::string add_recvfrom = extractSection(
        reactor_cc,
        "int IOUringReactor::addRecvFrom(IOController* controller)",
        "std::expected<void, IOError> IOUringReactor::initializeRecvFromBufferPool()");
    ok = requireText(add_recvfrom, "ioErrorCodeFromError(error)",
                     "expected lazy UDP pool failures to preserve the framework error") && ok;
    ok = requireText(add_recvfrom, "systemCodeFromError(error)",
                     "expected lazy UDP pool failures to preserve the system error") && ok;

    const std::string completion = extractSection(
        reactor_cc,
        "void IOUringReactor::processRecvFromCompletion",
        "}  // namespace galay::kernel");
    ok = requireText(completion, "cqe->res == -EINVAL && !m_recvmsg_multishot_confirmed",
                     "expected EINVAL to disable multishot only before capability confirmation") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "T140-IOUringUdpMultishotSourceCase PASS\n";
    return 0;
}
