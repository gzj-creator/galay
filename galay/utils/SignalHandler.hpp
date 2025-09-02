#ifndef GALAY_SIGNALHANDLER_HPP
#define GALAY_SIGNALHANDLER_HPP

#include <memory>
#include <functional>
#include <unordered_map>
#include <list>
#include <signal.h>
#include <mutex>
#include <concepts>

namespace galay::utils
{
    class SignalHandler
    {
    public:
        using SignalHandlerFunc = std::function<void(int)>;
        
        template <int ...Signals>
        static void setSignalHandler(SignalHandlerFunc func);
        
        // 单个信号注册
        static void setSignalHandler(int signal, SignalHandlerFunc func);
        
        // 移除信号处理
        static void removeSignalHandler(int signal);
        
        // 恢复默认信号处理
        static void restoreDefaultHandler(int signal);
        
    private:
        static void signalHandler(int sig);
        static std::unordered_map<int, SignalHandlerFunc> s_handlers;
        static std::mutex s_mutex;
    };

    template <int ...Signals>
    inline void SignalHandler::setSignalHandler(SignalHandlerFunc func)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto registerSignal = [&func](int signal) {
            s_handlers[signal] = func;
            struct sigaction sa;
            sa.sa_handler = SignalHandler::signalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;  // 自动重启被中断的系统调用
            sigaction(signal, &sa, nullptr);
        };
        
        // 展开参数包
        (registerSignal(Signals), ...);
    }
    
    // 单个信号注册的实现
    inline void SignalHandler::setSignalHandler(int signal, SignalHandlerFunc func)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_handlers[signal] = func;
        struct sigaction sa;
        sa.sa_handler = SignalHandler::signalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(signal, &sa, nullptr);
    }
    
    inline void SignalHandler::removeSignalHandler(int signal)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_handlers.erase(signal);
        ::signal(signal, SIG_DFL);  // 恢复默认处理
    }
    
    inline void SignalHandler::restoreDefaultHandler(int signal)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_handlers.erase(signal);
        ::signal(signal, SIG_DFL);
    }
    
    inline void SignalHandler::signalHandler(int sig)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_handlers.find(sig);
        if (it != s_handlers.end() && it->second) {
            it->second(sig);
        }
    }

    inline std::unordered_map<int, SignalHandler::SignalHandlerFunc> SignalHandler::s_handlers;
    inline std::mutex SignalHandler::s_mutex;
}

#endif