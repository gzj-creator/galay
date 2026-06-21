/**
 * @file t123_runtime_expected_src.cc
 * @brief 用途：锁定 Runtime 调用链通过 std::expected 返回错误，不使用 throw/try/catch。
 * 关键覆盖点：runtime、task、blocking_executor 边界不包含抛异常或捕获异常源码。
 * 通过条件：Runtime 调用链只通过返回值传播错误，测试返回 0。
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRoot()
{
    return std::filesystem::path(GALAY_SOURCE_ROOT);
}

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

void checkRuntimeSource(const std::filesystem::path& path,
                        const std::string& text,
                        std::vector<std::string>& failures)
{
    if (text.empty()) {
        failures.push_back(path.string() + ": failed to read source");
        return;
    }
    if (contains(text, "throw ")) {
        failures.push_back(path.string() + ": runtime API boundary must not throw");
    }
    if (contains(text, "@throws")) {
        failures.push_back(path.string() + ": runtime API docs must not advertise throwing");
    }
    if (contains(text, "std::runtime_error")) {
        failures.push_back(path.string() + ": runtime API boundary must not depend on runtime_error");
    }
    if (contains(text, "try {")) {
        failures.push_back(path.string() + ": runtime API boundary must not use try/catch");
    }
    if (contains(text, "catch (")) {
        failures.push_back(path.string() + ": runtime API boundary must not catch exceptions");
    }
}

}  // namespace

int main()
{
    const auto root = projectRoot();
    const std::vector<std::filesystem::path> source_paths = {
        root / "galay-kernel" / "core" / "runtime.h",
        root / "galay-kernel" / "core" / "runtime.cc",
        root / "galay-kernel" / "core" / "task.h",
        root / "galay-kernel" / "core" / "task.cc",
        root / "galay-kernel" / "core" / "blocking_executor.h",
        root / "galay-kernel" / "core" / "blocking_executor.cc",
    };

    std::vector<std::string> failures;
    for (const auto& source_path : source_paths) {
        checkRuntimeSource(source_path, readAll(source_path), failures);
    }

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << "[T123] " << failure << '\n';
        }
        return 1;
    }

    std::cout << "T123-RuntimeExpectedSourceBoundary PASS\n";
    return 0;
}
