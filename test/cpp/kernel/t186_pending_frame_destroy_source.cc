#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

bool require_contains(const std::string& name,
                     const std::string& text,
                     const char* needle)
{
    if (contains(text, needle)) {
        return true;
    }
    std::cerr << "[T186] " << name << " missing: " << needle << '\n';
    return false;
}

} // namespace

int main()
{
#ifdef GALAY_SOURCE_ROOT
    const std::filesystem::path root(GALAY_SOURCE_ROOT);
#else
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
        .parent_path().parent_path() / "src/cpp";
#endif
    const auto task = read_all(root / "galay-kernel/core/task.cc");
    const auto awaitable = read_all(root / "galay-kernel/core/awaitable.h");
    const auto epoll = read_all(root / "galay-kernel/core/epoll_reactor.cc");
    const auto runtime = read_all(root / "galay-kernel/core/runtime.cc");
    if (task.empty() || awaitable.empty() || epoll.empty() || runtime.empty()) {
        std::cerr << "[T186] failed to read lifecycle sources\n";
        return 1;
    }

    bool ok = true;
    ok = require_contains("task", task, "destroy_task_frame_for_state_teardown(this);") && ok;
    ok = require_contains("task", task, "bool destroy_task_frame(TaskState* state) noexcept") && ok;
    ok = require_contains("awaitable", awaitable,
                         "awaitable.m_controller->m_awaitable[IOController::READ]") && ok;
    ok = require_contains("awaitable", awaitable,
                         "awaitable.m_controller->remove_awaitable(Event)") && ok;
    ok = require_contains("epoll", epoll, "controller->m_awaitable[IOController::READ] = nullptr;") && ok;
    ok = require_contains("epoll", epoll, "detach_accept(*awaitable)") && ok;
    ok = require_contains("epoll", epoll, "std::move(*accept_resume).resume();") && ok;
    if (contains(epoll, "accept_waker.wake_up();") ||
        contains(epoll, "awaitable->m_result = std::unexpected(IOError(kClosed, 0))")) {
        std::cerr << "[T186] epoll accept bypasses OperationCompletion\n";
        ok = false;
    }
    ok = require_contains("runtime", runtime, "(*it)->stop();") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "T186-PendingFrameDestroySource PASS\n";
    return 0;
}
