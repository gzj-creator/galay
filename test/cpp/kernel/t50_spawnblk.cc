/**
 * @file t50_spawnblk.cc
 * @brief 用途：验证 `Runtime::spawnBlocking` 可在线程池中执行阻塞任务。
 * 关键覆盖点：阻塞 callable 提交、异步等待结果、与运行时线程分离执行。
 * 通过条件：阻塞任务结果正确返回，测试返回 0。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;

namespace {

Runtime makeBlockingRuntime()
{
    return RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();
}

int runThrowingIntCallableCase()
{
    Runtime runtime = makeBlockingRuntime();
    runtime.start();

    auto handle = runtime.spawnBlocking([]() -> int {
        throw std::runtime_error("blocking int callable failed");
    });
    assert(handle.has_value());

    auto result = handle->join();
    runtime.stop();

    assert(!result.has_value());
    assert(result.error().code() == detail::TaskResultErrorCode::kTaskException);
    return 0;
}

int runThrowingVoidCallableCase()
{
    Runtime runtime = makeBlockingRuntime();
    runtime.start();

    auto handle = runtime.spawnBlocking([]() -> void {
        throw std::runtime_error("blocking void callable failed");
    });
    assert(handle.has_value());

    auto result = handle->join();
    runtime.stop();

    assert(!result.has_value());
    assert(result.error().code() == detail::TaskResultErrorCode::kTaskException);
    return 0;
}

bool childCasePasses(const char* self, const char* case_name)
{
    pid_t child = fork();
    if (child < 0) {
        std::perror("[T50] fork");
        return false;
    }

    if (child == 0) {
        execl(self, self, case_name, static_cast<char*>(nullptr));
        std::perror("[T50] execl");
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::perror("[T50] waitpid");
        return false;
    }

    if (WIFSIGNALED(status)) {
        std::cerr << "[T50] " << case_name << " terminated by signal "
                  << WTERMSIG(status) << "\n";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "[T50] " << case_name << " exit code "
                  << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--throwing-int") == 0) {
        return runThrowingIntCallableCase();
    }
    if (argc == 2 && std::strcmp(argv[1], "--throwing-void") == 0) {
        return runThrowingVoidCallableCase();
    }

    Runtime runtime = makeBlockingRuntime();
    runtime.start();

    auto start = std::chrono::steady_clock::now();

    auto first = runtime.spawnBlocking([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 11;
    });
    auto second = runtime.spawnBlocking([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 31;
    });
    assert(first.has_value());
    assert(second.has_value());

    const auto first_result = first->join();
    const auto second_result = second->join();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    assert(first_result.has_value());
    assert(second_result.has_value());
    assert(*first_result == 11);
    assert(*second_result == 31);
    assert(elapsed_ms < 190 && "spawnBlocking tasks should execute concurrently");

    runtime.stop();

    assert(childCasePasses(argv[0], "--throwing-int"));
    assert(childCasePasses(argv[0], "--throwing-void"));

    std::cout << "T50-RuntimeSpawnBlocking PASS\n";
    return 0;
}
