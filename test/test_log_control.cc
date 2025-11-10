#include "galay/common/Log.h"
#include <cstdio>
#include <iostream>
#include <thread>
#include <chrono>
#include <string>

using namespace galay;

std::string levelToString(spdlog::level::level_enum level) {
    auto sv = spdlog::level::to_string_view(level);
    return std::string(sv.data(), sv.size());
}

void testLogControl() {
    std::cout << "\n=== 测试 1: 默认状态（应该是禁用） ===" << std::endl;
    std::cout << "日志状态: " << (log::isEnabled() ? "启用" : "禁用") << std::endl;
    std::cout << "日志级别: " << levelToString(log::getLevel()) << std::endl;
    
    // 尝试输出日志（不应该有输出）
    LogInfo("这条日志不应该显示（日志已禁用）");
    LogDebug("这条日志不应该显示（日志已禁用）");
    
    std::cout << "\n=== 测试 2: 启用日志（debug 级别） ===" << std::endl;
    log::enable(spdlog::level::debug);
    std::cout << "日志状态: " << (log::isEnabled() ? "启用" : "禁用") << std::endl;
    std::cout << "日志级别: " << levelToString(log::getLevel()) << std::endl;
    
    // 现在应该能看到日志
    LogDebug("这是一条 debug 日志");
    LogInfo("这是一条 info 日志");
    LogWarn("这是一条 warn 日志");
    LogError("这是一条 error 日志");
    
    std::cout << "\n=== 测试 3: 调整日志级别为 info ===" << std::endl;
    log::setLevel(spdlog::level::info);
    std::cout << "日志级别: " << levelToString(log::getLevel()) << std::endl;
    
    // debug 日志不应该显示，info 及以上应该显示
    LogDebug("这条 debug 日志不应该显示");
    LogInfo("这条 info 日志应该显示");
    LogWarn("这条 warn 日志应该显示");
    
    std::cout << "\n=== 测试 4: 调整日志级别为 warn ===" << std::endl;
    log::setLevel(spdlog::level::warn);
    std::cout << "日志级别: " << levelToString(log::getLevel()) << std::endl;
    
    // 只有 warn 及以上应该显示
    LogDebug("这条 debug 日志不应该显示");
    LogInfo("这条 info 日志不应该显示");
    LogWarn("这条 warn 日志应该显示");
    LogError("这条 error 日志应该显示");
    
    std::cout << "\n=== 测试 5: 禁用日志 ===" << std::endl;
    log::disable();
    std::cout << "日志状态: " << (log::isEnabled() ? "启用" : "禁用") << std::endl;
    std::cout << "日志级别: " << levelToString(log::getLevel()) << std::endl;
    
    // 所有日志都不应该显示
    LogDebug("这条日志不应该显示（日志已禁用）");
    LogInfo("这条日志不应该显示（日志已禁用）");
    LogWarn("这条日志不应该显示（日志已禁用）");
    LogError("这条日志不应该显示（日志已禁用）");
    
    std::cout << "\n=== 测试 6: 重新启用日志 ===" << std::endl;
    log::enable(spdlog::level::info);
    std::cout << "日志状态: " << (log::isEnabled() ? "启用" : "禁用") << std::endl;
    
    LogInfo("日志已重新启用");
    
    // 等待异步日志刷新
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int main() {
    std::cout << "Galay 框架日志控制测试" << std::endl;
    std::cout << "======================================" << std::endl;
    
    testLogControl();
    
    std::cout << "\n======================================" << std::endl;
    std::cout << "测试完成！" << std::endl;
    std::cout << "请检查 logs/galay.log 文件查看日志输出" << std::endl;
    
    // 关闭日志系统
    details::InternelLogger::getInstance()->shutdown();
    
    return 0;
}

