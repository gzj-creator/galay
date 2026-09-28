#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef GALAY_SOURCE_ROOT
#define GALAY_SOURCE_ROOT "src/cpp"
#endif

namespace {

std::string readAll(const std::filesystem::path& path)
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

bool requireContains(const std::string& name,
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
    std::filesystem::path root(GALAY_SOURCE_ROOT);
    if (!std::filesystem::exists(root / "galay-kernel")) {
        const auto cwd = std::filesystem::current_path();
        const std::filesystem::path candidates[] = {
            cwd / "src/cpp",
            cwd / "../../src/cpp",
            cwd / "../../../src/cpp",
            cwd / "../../../../src/cpp",
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate / "galay-kernel")) {
                root = candidate;
                break;
            }
        }
    }
    const auto task = readAll(root / "galay-kernel/core/task.cc");
    const auto awaitable = readAll(root / "galay-kernel/core/awaitable.h");
    const auto epoll = readAll(root / "galay-kernel/core/epoll_reactor.cc");
    const auto runtime = readAll(root / "galay-kernel/core/runtime.cc");
    if (task.empty() || awaitable.empty() || epoll.empty() || runtime.empty()) {
        std::cerr << "[T186] failed to read lifecycle sources\n";
        return 1;
    }

    bool ok = true;
    ok = requireContains("task", task, "destroyTaskFrameForStateTeardown(this);") && ok;
    ok = requireContains("task", task, "bool destroyTaskFrame(TaskState* state) noexcept") && ok;
    ok = requireContains("awaitable", awaitable,
                         "awaitable.m_controller->m_awaitable[IOController::READ]") && ok;
    ok = requireContains("awaitable", awaitable,
                         "awaitable.m_controller->removeAwaitable(Event)") && ok;
    ok = requireContains("epoll", epoll, "controller->m_awaitable[IOController::READ] = nullptr;") && ok;
    ok = requireContains("epoll", epoll, "detachAccept(*awaitable)") && ok;
    ok = requireContains("epoll", epoll, "std::move(*accept_resume).resume();") && ok;
    if (contains(epoll, "accept_waker.wakeUp();") ||
        contains(epoll, "awaitable->m_result = std::unexpected(IOError(kClosed, 0))")) {
        std::cerr << "[T186] epoll accept bypasses OperationCompletion\n";
        ok = false;
    }
    ok = requireContains("runtime", runtime, "(*it)->stop();") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "T186-PendingFrameDestroySource PASS\n";
    return 0;
}
