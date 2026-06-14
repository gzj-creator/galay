/**
 * @file backtrace.hpp
 * @brief 堆栈跟踪工具
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供堆栈跟踪获取、打印和崩溃信号处理功能，
 *          支持 macOS 和 Linux 平台的符号反解。
 */

#ifndef GALAY_UTILS_BACKTRACE_HPP
#define GALAY_UTILS_BACKTRACE_HPP

#include "utils/common/defn.hpp"
#include "utils/core/type_name.hpp"
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <csignal>

#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#include <dlfcn.h>
#endif

namespace galay::utils {

/**
 * @brief 堆栈跟踪工具类
 * @details 提供堆栈跟踪获取、打印和崩溃信号处理功能。
 */
class BackTrace {
public:
    /**
     * @brief 获取当前调用堆栈
     * @param maxFrames 最大栈帧数（默认 64）
     * @param skipFrames 跳过的栈帧数（默认 1）
     * @return 堆栈字符串向量
     */
    static std::vector<std::string> getStackTrace(int maxFrames = 64, int skipFrames = 1) {
        std::vector<std::string> result;

#if defined(__APPLE__) || defined(__linux__)
        std::vector<void*> buffer(maxFrames);
        int numFrames = backtrace(buffer.data(), maxFrames);

        if (numFrames <= skipFrames) {
            return result;
        }

        char** symbols = backtrace_symbols(buffer.data(), numFrames);
        if (!symbols) {
            return result;
        }

        for (int i = skipFrames; i < numFrames; ++i) {
            std::string frame = symbols[i];

#if defined(__APPLE__)
            size_t start = frame.find(" _");
            size_t end = frame.find(" + ");
            if (start != std::string::npos && end != std::string::npos) {
                std::string mangledName = frame.substr(start + 1, end - start - 1);
                std::string demangled = demangleSymbol(mangledName.c_str());
                if (demangled != mangledName) {
                    frame = frame.substr(0, start + 1) + demangled + frame.substr(end);
                }
            }
#elif defined(__linux__)
            size_t start = frame.find('(');
            size_t end = frame.find('+');
            if (start != std::string::npos && end != std::string::npos && end > start) {
                std::string mangledName = frame.substr(start + 1, end - start - 1);
                std::string demangled = demangleSymbol(mangledName.c_str());
                if (demangled != mangledName) {
                    frame = frame.substr(0, start + 1) + demangled + frame.substr(end);
                }
            }
#endif

            result.push_back(frame);
        }

        std::free(symbols);
#endif

        return result;
    }

    /**
     * @brief 打印堆栈跟踪到标准错误流
     * @param maxFrames 最大栈帧数（默认 64）
     * @param skipFrames 跳过的栈帧数（默认 1）
     */
    static void printStackTrace(int maxFrames = 64, int skipFrames = 1) {
        auto frames = getStackTrace(maxFrames, skipFrames + 1);

        std::cerr << "Stack trace (" << frames.size() << " frames):" << std::endl;
        for (size_t i = 0; i < frames.size(); ++i) {
            std::cerr << "  #" << i << " " << frames[i] << std::endl;
        }
    }

    /**
     * @brief 获取堆栈跟踪字符串
     * @param maxFrames 最大栈帧数（默认 64）
     * @param skipFrames 跳过的栈帧数（默认 1）
     * @return 格式化的堆栈跟踪字符串
     */
    static std::string getStackTraceString(int maxFrames = 64, int skipFrames = 1) {
        auto frames = getStackTrace(maxFrames, skipFrames + 1);

        std::ostringstream oss;
        oss << "Stack trace (" << frames.size() << " frames):\n";
        for (size_t i = 0; i < frames.size(); ++i) {
            oss << "  #" << i << " " << frames[i] << "\n";
        }

        return oss.str();
    }

    /**
     * @brief 安装崩溃信号处理器（SIGSEGV、SIGABRT、SIGFPE、SIGILL、SIGBUS）
     */
    static void installCrashHandlers() {
        std::signal(SIGSEGV, crashSignalHandler);
        std::signal(SIGABRT, crashSignalHandler);
        std::signal(SIGFPE, crashSignalHandler);
        std::signal(SIGILL, crashSignalHandler);
#if defined(SIGBUS)
        std::signal(SIGBUS, crashSignalHandler);
#endif
    }

private:
    static void crashSignalHandler(int signal) {
        const char* signalName = "Unknown";
        switch (signal) {
            case SIGSEGV: signalName = "SIGSEGV (Segmentation fault)"; break;
            case SIGABRT: signalName = "SIGABRT (Abort)"; break;
            case SIGFPE:  signalName = "SIGFPE (Floating point exception)"; break;
            case SIGILL:  signalName = "SIGILL (Illegal instruction)"; break;
#if defined(SIGBUS)
            case SIGBUS:  signalName = "SIGBUS (Bus error)"; break;
#endif
            default: break;
        }

        std::cerr << "\n=== CRASH DETECTED ===" << std::endl;
        std::cerr << "Signal: " << signalName << " (" << signal << ")" << std::endl;
        printStackTrace(64, 2);
        std::cerr << "======================" << std::endl;

        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_BACKTRACE_HPP
