/**
 * @file process.hpp
 * @brief 进程管理工具
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供跨平台的进程管理功能，包括进程 ID 获取、子进程创建、
 *          进程等待、命令执行、守护进程化等。支持 Windows 和 POSIX 平台。
 */

#ifndef GALAY_UTILS_PROCESS_HPP
#define GALAY_UTILS_PROCESS_HPP

#include "../common/defn.hpp"
#include "system.hpp"
#include <algorithm>
#include <cerrno>
#include <expected>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#endif

namespace galay::utils {

#if defined(_WIN32)
using ProcessId = DWORD; ///< Windows 进程 ID 类型
#else
using ProcessId = pid_t; ///< POSIX 进程 ID 类型
#endif

/**
 * @brief 进程退出状态
 */
struct ExitStatus {
    int code; ///< 退出码
    int signal; ///< 终止信号编号
    bool signaled; ///< 是否被信号终止

    /**
     * @brief 判断是否成功退出
     * @return 未被信号终止且退出码为 0 返回 true
     */
    bool success() const { return !signaled && code == 0; }
};

/**
 * @brief 进程优先级操作错误
 */
enum class ProcessPriorityError {
    InvalidPriority, ///< 优先级超出支持范围
    PermissionDenied, ///< 当前进程没有权限修改目标进程优先级
    NotFound, ///< 目标进程不存在
    SystemError ///< 其他系统错误
};

/**
 * @brief 获取进程优先级错误描述
 * @param error 错误枚举值
 * @return 静态错误描述字符串
 */
inline constexpr const char* processPriorityErrorString(ProcessPriorityError error) noexcept {
    switch (error) {
    case ProcessPriorityError::InvalidPriority:
        return "invalid priority";
    case ProcessPriorityError::PermissionDenied:
        return "permission denied";
    case ProcessPriorityError::NotFound:
        return "process not found";
    case ProcessPriorityError::SystemError:
        return "system error";
    }
    return "unknown process priority error";
}

/**
 * @brief 进程 CPU 亲和性操作错误
 */
enum class ProcessAffinityError {
    EmptyCpuSet, ///< CPU 集合为空
    InvalidCpu, ///< CPU ID 超出当前系统支持范围
    PermissionDenied, ///< 当前进程没有权限修改目标进程亲和性
    NotFound, ///< 目标进程不存在
    Unsupported, ///< 当前平台不支持进程级 CPU 亲和性
    SystemError ///< 其他系统错误
};

/**
 * @brief 获取进程 CPU 亲和性错误描述
 * @param error 错误枚举值
 * @return 静态错误描述字符串
 */
inline constexpr const char* processAffinityErrorString(ProcessAffinityError error) noexcept {
    switch (error) {
    case ProcessAffinityError::EmptyCpuSet:
        return "empty cpu set";
    case ProcessAffinityError::InvalidCpu:
        return "invalid cpu";
    case ProcessAffinityError::PermissionDenied:
        return "permission denied";
    case ProcessAffinityError::NotFound:
        return "process not found";
    case ProcessAffinityError::Unsupported:
        return "unsupported";
    case ProcessAffinityError::SystemError:
        return "system error";
    }
    return "unknown process affinity error";
}

/**
 * @brief 进程管理工具类
 * @details 提供跨平台的进程 ID 获取、子进程创建、进程等待、命令执行和守护进程化等静态方法。
 */
class Process {
public:
    /**
     * @brief 获取当前进程 ID
     * @return 当前进程 ID
     */
    static ProcessId currentId() {
#if defined(_WIN32)
        return GetCurrentProcessId();
#else
        return getpid();
#endif
    }

    /**
     * @brief 获取父进程 ID
     * @return 父进程 ID
     */
    static ProcessId parentId() {
#if defined(_WIN32)
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) {
            return 0;
        }

        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        DWORD currentPid = GetCurrentProcessId();
        DWORD parentPid = 0;

        if (Process32First(hSnapshot, &pe)) {
            do {
                if (pe.th32ProcessID == currentPid) {
                    parentPid = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32Next(hSnapshot, &pe));
        }

        CloseHandle(hSnapshot);
        return parentPid;
#else
        return getppid();
#endif
    }

    /**
     * @brief 获取当前进程优先级
     * @return POSIX nice 值语义的优先级，范围为 [-20, 19]；失败返回具体错误
     * @note Windows 平台会把进程 priority class 映射为近似 nice 值。
     */
    [[nodiscard]] static std::expected<int, ProcessPriorityError> priority() {
        return priority(currentId());
    }

    /**
     * @brief 获取指定进程优先级
     * @param pid 目标进程 ID
     * @return POSIX nice 值语义的优先级，范围为 [-20, 19]；失败返回具体错误
     * @note Windows 平台会把进程 priority class 映射为近似 nice 值。
     */
    [[nodiscard]] static std::expected<int, ProcessPriorityError> priority(ProcessId pid) {
#if defined(_WIN32)
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return std::unexpected(priorityErrorFromWindows(GetLastError()));
        }

        DWORD priorityClass = GetPriorityClass(hProcess);
        DWORD priorityError = priorityClass == 0 ? GetLastError() : ERROR_SUCCESS;
        BOOL closeResult = CloseHandle(hProcess);
        if (priorityClass == 0) {
            return std::unexpected(priorityErrorFromWindows(priorityError));
        }
        if (closeResult == 0) {
            return std::unexpected(ProcessPriorityError::SystemError);
        }

        switch (priorityClass) {
        case IDLE_PRIORITY_CLASS:
            return 19;
        case BELOW_NORMAL_PRIORITY_CLASS:
            return 10;
        case NORMAL_PRIORITY_CLASS:
            return 0;
        case ABOVE_NORMAL_PRIORITY_CLASS:
            return -5;
        case HIGH_PRIORITY_CLASS:
            return -10;
        case REALTIME_PRIORITY_CLASS:
            return -20;
        default:
            return std::unexpected(ProcessPriorityError::SystemError);
        }
#else
        errno = 0;
        int value = getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
        if (value == -1 && errno != 0) {
            return std::unexpected(priorityErrorFromErrno(errno));
        }
        return value;
#endif
    }

    /**
     * @brief 设置当前进程优先级
     * @param value POSIX nice 值语义的优先级，范围为 [-20, 19]；数值越小优先级越高
     * @return 成功返回空 expected，失败返回具体错误
     */
    [[nodiscard]] static std::expected<void, ProcessPriorityError> setPriority(int value) {
        return setPriority(currentId(), value);
    }

    /**
     * @brief 设置指定进程优先级
     * @param pid 目标进程 ID
     * @param value POSIX nice 值语义的优先级，范围为 [-20, 19]；数值越小优先级越高
     * @return 成功返回空 expected，失败返回具体错误
     * @note POSIX 平台降低 nice 值通常需要额外权限；Windows 平台会映射到进程 priority class。
     */
    [[nodiscard]] static std::expected<void, ProcessPriorityError> setPriority(ProcessId pid, int value) {
        if (value < -20 || value > 19) {
            return std::unexpected(ProcessPriorityError::InvalidPriority);
        }

#if defined(_WIN32)
        HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return std::unexpected(priorityErrorFromWindows(GetLastError()));
        }

        DWORD priorityClass = NORMAL_PRIORITY_CLASS;
        if (value <= -15) {
            priorityClass = REALTIME_PRIORITY_CLASS;
        } else if (value <= -10) {
            priorityClass = HIGH_PRIORITY_CLASS;
        } else if (value <= -5) {
            priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
        } else if (value == 0) {
            priorityClass = NORMAL_PRIORITY_CLASS;
        } else if (value <= 10) {
            priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
        } else {
            priorityClass = IDLE_PRIORITY_CLASS;
        }

        BOOL setResult = SetPriorityClass(hProcess, priorityClass);
        DWORD setError = setResult == 0 ? GetLastError() : ERROR_SUCCESS;
        BOOL closeResult = CloseHandle(hProcess);
        if (setResult == 0) {
            return std::unexpected(priorityErrorFromWindows(setError));
        }
        if (closeResult == 0) {
            return std::unexpected(ProcessPriorityError::SystemError);
        }
        return {};
#else
        if (setpriority(PRIO_PROCESS, static_cast<id_t>(pid), value) != 0) {
            return std::unexpected(priorityErrorFromErrno(errno));
        }
        return {};
#endif
    }

    /**
     * @brief 获取当前进程 CPU 亲和性
     * @return 当前允许运行的零基 CPU ID 列表；失败返回具体错误
     * @note Linux/Windows 支持进程级 CPU 亲和性；不支持的平台返回 Unsupported。
     */
    [[nodiscard]] static std::expected<std::vector<unsigned int>, ProcessAffinityError> cpuAffinity() {
        return cpuAffinity(currentId());
    }

    /**
     * @brief 获取指定进程 CPU 亲和性
     * @param pid 目标进程 ID
     * @return 目标进程允许运行的零基 CPU ID 列表；失败返回具体错误
     */
    [[nodiscard]] static std::expected<std::vector<unsigned int>, ProcessAffinityError> cpuAffinity(ProcessId pid) {
#if defined(_WIN32)
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return std::unexpected(affinityErrorFromWindows(GetLastError()));
        }

        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        BOOL getResult = GetProcessAffinityMask(hProcess, &processMask, &systemMask);
        DWORD getError = getResult == 0 ? GetLastError() : ERROR_SUCCESS;
        BOOL closeResult = CloseHandle(hProcess);
        if (getResult == 0) {
            return std::unexpected(affinityErrorFromWindows(getError));
        }
        if (closeResult == 0) {
            return std::unexpected(ProcessAffinityError::SystemError);
        }

        std::vector<unsigned int> cpus;
        for (unsigned int cpu = 0; cpu < sizeof(DWORD_PTR) * 8U; ++cpu) {
            if ((processMask & (static_cast<DWORD_PTR>(1) << cpu)) != 0) {
                cpus.push_back(cpu);
            }
        }
        return cpus;
#elif defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (sched_getaffinity(static_cast<pid_t>(pid), sizeof(mask), &mask) != 0) {
            return std::unexpected(affinityErrorFromErrno(errno));
        }

        std::vector<unsigned int> cpus;
        const unsigned int cpu_count = System::cpuCount();
        for (unsigned int cpu = 0; cpu < cpu_count && cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &mask)) {
                cpus.push_back(cpu);
            }
        }
        return cpus;
#else
        (void)pid;
        return std::unexpected(ProcessAffinityError::Unsupported);
#endif
    }

    /**
     * @brief 设置当前进程 CPU 亲和性
     * @param cpus 零基 CPU ID 集合，不能为空
     * @return 成功返回空 expected，失败返回具体错误
     */
    [[nodiscard]] static std::expected<void, ProcessAffinityError>
    setCpuAffinity(std::span<const unsigned int> cpus) {
        return setCpuAffinity(currentId(), cpus);
    }

    /**
     * @brief 设置指定进程 CPU 亲和性
     * @param pid 目标进程 ID
     * @param cpus 零基 CPU ID 集合，不能为空
     * @return 成功返回空 expected，失败返回具体错误
     * @note Linux/Windows 支持进程级 CPU 亲和性；不支持的平台返回 Unsupported。
     */
    [[nodiscard]] static std::expected<void, ProcessAffinityError>
    setCpuAffinity(ProcessId pid, std::span<const unsigned int> cpus) {
        auto normalized = normalizeCpuSet(cpus);
        if (!normalized.has_value()) {
            return std::unexpected(normalized.error());
        }

#if defined(_WIN32)
        DWORD_PTR processMask = 0;
        for (unsigned int cpu : *normalized) {
            if (cpu >= sizeof(DWORD_PTR) * 8U) {
                return std::unexpected(ProcessAffinityError::InvalidCpu);
            }
            processMask |= (static_cast<DWORD_PTR>(1) << cpu);
        }

        HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return std::unexpected(affinityErrorFromWindows(GetLastError()));
        }

        BOOL setResult = SetProcessAffinityMask(hProcess, processMask);
        DWORD setError = setResult == 0 ? GetLastError() : ERROR_SUCCESS;
        BOOL closeResult = CloseHandle(hProcess);
        if (setResult == 0) {
            return std::unexpected(affinityErrorFromWindows(setError));
        }
        if (closeResult == 0) {
            return std::unexpected(ProcessAffinityError::SystemError);
        }
        return {};
#elif defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (unsigned int cpu : *normalized) {
            if (cpu >= CPU_SETSIZE) {
                return std::unexpected(ProcessAffinityError::InvalidCpu);
            }
            CPU_SET(cpu, &mask);
        }
        if (sched_setaffinity(static_cast<pid_t>(pid), sizeof(mask), &mask) != 0) {
            return std::unexpected(affinityErrorFromErrno(errno));
        }
        return {};
#else
        (void)pid;
        return std::unexpected(ProcessAffinityError::Unsupported);
#endif
    }

    /**
     * @brief 等待指定进程结束
     * @param pid 目标进程 ID
     * @param options 等待选项（WNOHANG 等）
     * @return 退出状态，失败返回 std::nullopt
     */
    static std::optional<ExitStatus> wait(ProcessId pid, int options = 0) {
#if defined(_WIN32)
        HANDLE hProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return std::nullopt;
        }

        DWORD waitResult = WaitForSingleObject(hProcess, (options & 1) ? 0 : INFINITE);
        if (waitResult == WAIT_TIMEOUT) {
            CloseHandle(hProcess);
            return std::nullopt;
        }

        DWORD exitCode;
        GetExitCodeProcess(hProcess, &exitCode);
        CloseHandle(hProcess);

        return ExitStatus{static_cast<int>(exitCode), 0, false};
#else
        int status;
        pid_t result = waitpid(pid, &status, options);

        if (result <= 0) {
            return std::nullopt;
        }

        ExitStatus exitStatus{0, 0, false};

        if (WIFEXITED(status)) {
            exitStatus.code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exitStatus.signaled = true;
            exitStatus.signal = WTERMSIG(status);
        }

        return exitStatus;
#endif
    }

    /**
     * @brief 创建子进程执行指定程序
     * @param path 可执行文件路径
     * @param args 命令行参数列表
     * @return 子进程 ID，失败返回 -1
     */
    static ProcessId spawn(const std::string& path, const std::vector<std::string>& args) {
#if defined(_WIN32)
        std::string cmdLine = "\"" + path + "\"";
        for (const auto& arg : args) {
            cmdLine += " \"" + arg + "\"";
        }

        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;

        if (!CreateProcessA(nullptr, const_cast<char*>(cmdLine.c_str()),
                            nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            return static_cast<ProcessId>(-1);
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
#else
        pid_t pid = fork();

        if (pid < 0) {
            return static_cast<ProcessId>(-1);
        }

        if (pid == 0) {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(path.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            execv(path.c_str(), argv.data());
            _exit(127);
        }

        return pid;
#endif
    }

    /**
     * @brief 执行 shell 命令并等待完成
     * @param command shell 命令字符串
     * @return 退出状态
     */
    static ExitStatus execute(const std::string& command) {
#if defined(_WIN32)
        int result = std::system(command.c_str());
        return ExitStatus{result, 0, false};
#else
        int result = std::system(command.c_str());

        ExitStatus status{0, 0, false};
        if (WIFEXITED(result)) {
            status.code = WEXITSTATUS(result);
        } else if (WIFSIGNALED(result)) {
            status.signaled = true;
            status.signal = WTERMSIG(result);
        }

        return status;
#endif
    }

    /**
     * @brief 执行 shell 命令并捕获标准输出
     * @param command shell 命令字符串
     * @return 退出状态和标准输出内容的键值对
     */
    static std::pair<ExitStatus, std::string> executeWithOutput(const std::string& command) {
        std::string output;
        ExitStatus status{0, 0, false};

#if defined(_WIN32)
        FILE* pipe = _popen(command.c_str(), "r");
#else
        FILE* pipe = popen(command.c_str(), "r");
#endif

        if (!pipe) {
            status.code = -1;
            return {status, output};
        }

        std::array<char, 128> buffer;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }

#if defined(_WIN32)
        int result = _pclose(pipe);
        status.code = result;
#else
        int result = pclose(pipe);
        if (WIFEXITED(result)) {
            status.code = WEXITSTATUS(result);
        } else if (WIFSIGNALED(result)) {
            status.signaled = true;
            status.signal = WTERMSIG(result);
        }
#endif

        return {status, output};
    }

    /**
     * @brief 向进程发送信号
     * @param pid 目标进程 ID
     * @param signal 信号编号
     * @return 成功返回 true
     */
    static bool kill(ProcessId pid, int signal) {
#if defined(_WIN32)
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!hProcess) {
            return false;
        }

        BOOL result = TerminateProcess(hProcess, static_cast<UINT>(signal));
        CloseHandle(hProcess);
        return result != 0;
#else
        return ::kill(pid, signal) == 0;
#endif
    }

    /**
     * @brief 检查进程是否正在运行
     * @param pid 目标进程 ID
     * @return 正在运行返回 true
     */
    static bool isRunning(ProcessId pid) {
#if defined(_WIN32)
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProcess) {
            return false;
        }

        DWORD exitCode;
        BOOL result = GetExitCodeProcess(hProcess, &exitCode);
        CloseHandle(hProcess);

        return result && exitCode == STILL_ACTIVE;
#else
        return ::kill(pid, 0) == 0;
#endif
    }

    /**
     * @brief 将当前进程转为守护进程（仅 POSIX）
     * @return 成功返回 true
     */
    static bool daemonize() {
#if defined(_WIN32)
        return false;
#else
        pid_t pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid > 0) {
            _exit(0);
        }

        if (setsid() < 0) {
            return false;
        }

        pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid > 0) {
            _exit(0);
        }

        if (chdir("/") < 0) {
            return false;
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);

        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) {
                close(fd);
            }
        }

        return true;
#endif
    }

private:
    static std::expected<std::vector<unsigned int>, ProcessAffinityError>
    normalizeCpuSet(std::span<const unsigned int> cpus) {
        if (cpus.empty()) {
            return std::unexpected(ProcessAffinityError::EmptyCpuSet);
        }

        const unsigned int cpu_count = System::cpuCount();
        std::vector<unsigned int> normalized(cpus.begin(), cpus.end());
        std::sort(normalized.begin(), normalized.end());
        normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());

        for (unsigned int cpu : normalized) {
            if (cpu >= cpu_count) {
                return std::unexpected(ProcessAffinityError::InvalidCpu);
            }
        }
        return normalized;
    }

#if defined(_WIN32)
    static ProcessPriorityError priorityErrorFromWindows(DWORD error) {
        switch (error) {
        case ERROR_ACCESS_DENIED:
            return ProcessPriorityError::PermissionDenied;
        case ERROR_INVALID_PARAMETER:
        case ERROR_NOT_FOUND:
        case ERROR_INVALID_HANDLE:
            return ProcessPriorityError::NotFound;
        default:
            return ProcessPriorityError::SystemError;
        }
    }

    static ProcessAffinityError affinityErrorFromWindows(DWORD error) {
        switch (error) {
        case ERROR_ACCESS_DENIED:
            return ProcessAffinityError::PermissionDenied;
        case ERROR_INVALID_PARAMETER:
        case ERROR_NOT_FOUND:
        case ERROR_INVALID_HANDLE:
            return ProcessAffinityError::NotFound;
        default:
            return ProcessAffinityError::SystemError;
        }
    }
#else
    static ProcessPriorityError priorityErrorFromErrno(int error) {
        switch (error) {
        case EACCES:
        case EPERM:
            return ProcessPriorityError::PermissionDenied;
        case ESRCH:
            return ProcessPriorityError::NotFound;
        case EINVAL:
            return ProcessPriorityError::InvalidPriority;
        default:
            return ProcessPriorityError::SystemError;
        }
    }

    static ProcessAffinityError affinityErrorFromErrno(int error) {
        switch (error) {
        case EACCES:
        case EPERM:
            return ProcessAffinityError::PermissionDenied;
        case ESRCH:
            return ProcessAffinityError::NotFound;
        case EINVAL:
            return ProcessAffinityError::InvalidCpu;
        default:
            return ProcessAffinityError::SystemError;
        }
    }
#endif
};

} // namespace galay::utils

#endif // GALAY_UTILS_PROCESS_HPP
