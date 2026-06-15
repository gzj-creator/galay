#ifndef GALAY_UTILS_SIGNAL_HANDLER_HPP
#define GALAY_UTILS_SIGNAL_HANDLER_HPP

#include "galay-utils/common/defn.hpp"
#include <functional>
#include <unordered_map>
#include <mutex>
#include <csignal>

/**
 * @file signal.hpp
 * @brief 信号处理工具
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供跨平台的信号处理器注册、移除、忽略和阻塞功能。
 *          单例模式，支持自定义信号回调函数。
 */

namespace galay::utils {

/**
 * @brief 信号处理器工具类
 * @details 单例模式，提供信号处理器的注册、移除、忽略和阻塞功能。
 */
class SignalHandler {
public:
    /// 信号处理回调函数类型
    using Handler = std::function<void(int)>;

    /**
     * @brief 获取单例实例
     * @return 信号处理器实例引用
     */
    static SignalHandler& instance() {
        static SignalHandler instance;
        return instance;
    }

    /**
     * @brief 设置信号处理器
     * @param signal 信号编号
     * @param handler 处理回调函数
     * @return 设置成功返回 true
     */
    bool setHandler(int signal, Handler handler) {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_handlers[signal] = std::move(handler);

#if defined(_WIN32)
        auto result = std::signal(signal, &SignalHandler::signalCallback);
        return result != SIG_ERR;
#else
        struct sigaction sa{};
        sa.sa_handler = &SignalHandler::signalCallback;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        return sigaction(signal, &sa, nullptr) == 0;
#endif
    }

    /**
     * @brief 为多个信号设置同一个处理器
     * @tparam Signals 信号编号参数包
     * @param handler 处理回调函数
     * @return 所有信号均设置成功返回 true
     */
    template<int... Signals>
    bool setHandler(Handler handler) {
        bool success = true;
        ((success &= setHandler(Signals, handler)), ...);
        return success;
    }

    /**
     * @brief 移除信号处理器并恢复默认行为
     * @param signal 信号编号
     * @return 移除成功返回 true
     */
    bool removeHandler(int signal) {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_handlers.erase(signal);

#if defined(_WIN32)
        auto result = std::signal(signal, SIG_DFL);
        return result != SIG_ERR;
#else
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        return sigaction(signal, &sa, nullptr) == 0;
#endif
    }

    /**
     * @brief 恢复信号默认处理行为
     * @param signal 信号编号
     * @return 恢复成功返回 true
     */
    bool restoreDefault(int signal) {
        return removeHandler(signal);
    }

    /**
     * @brief 忽略指定信号
     * @param signal 信号编号
     * @return 设置成功返回 true
     */
    bool ignoreSignal(int signal) {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_handlers.erase(signal);

#if defined(_WIN32)
        auto result = std::signal(signal, SIG_IGN);
        return result != SIG_ERR;
#else
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        return sigaction(signal, &sa, nullptr) == 0;
#endif
    }

    /**
     * @brief 阻塞指定信号
     * @param signal 信号编号
     * @return 阻塞成功返回 true（Windows 不支持，返回 false）
     */
    bool blockSignal(int signal) {
#if defined(_WIN32)
        return false;
#else
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, signal);
        return pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0;
#endif
    }

    /**
     * @brief 取消阻塞指定信号
     * @param signal 信号编号
     * @return 取消成功返回 true（Windows 不支持，返回 false）
     */
    bool unblockSignal(int signal) {
#if defined(_WIN32)
        return false;
#else
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, signal);
        return pthread_sigmask(SIG_UNBLOCK, &set, nullptr) == 0;
#endif
    }

    /**
     * @brief 检查信号是否已注册处理器
     * @param signal 信号编号
     * @return 已注册返回 true
     */
    bool hasHandler(int signal) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_handlers.find(signal) != m_handlers.end();
    }

private:
    SignalHandler() = default;
    ~SignalHandler() = default;

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;

    static void signalCallback(int signal) {
        auto& self = instance();
        std::lock_guard<std::mutex> lock(self.m_mutex);

        auto it = self.m_handlers.find(signal);
        if (it != self.m_handlers.end() && it->second) {
            it->second(signal);
        }
    }

    mutable std::mutex m_mutex;
    std::unordered_map<int, Handler> m_handlers;
};

} // namespace galay::utils

#endif // GALAY_UTILS_SIGNAL_HANDLER_HPP
