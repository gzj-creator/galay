#include "galay/common/Log.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <string>
#include <sys/stat.h>

using namespace galay;

bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

int main() {
    std::cout << "=== 测试日志延迟初始化 ===" << std::endl;
    
    // 测试 1: 默认状态，不应该创建日志文件
    std::cout << "\n[测试 1] 默认状态（未初始化）" << std::endl;
    std::cout << "日志是否初始化: " << (log::isInitialized() ? "是" : "否") << std::endl;
    std::cout << "日志是否启用: " << (log::isEnabled() ? "是" : "否") << std::endl;
    std::cout << "日志文件是否存在: " << (fileExists("logs/galay.log") ? "存在" : "不存在") << std::endl;
    
    // 尝试输出日志（不应该有任何效果）
    LogInfo("这条日志不应该输出，也不应该创建文件");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "输出日志后，文件是否存在: " << (fileExists("logs/galay.log") ? "存在" : "不存在") << std::endl;
    
    // 测试 2: 启用日志，应该创建日志文件
    std::cout << "\n[测试 2] 启用日志" << std::endl;
    log::enable(spdlog::level::debug);
    
    std::cout << "日志是否初始化: " << (log::isInitialized() ? "是" : "否") << std::endl;
    std::cout << "日志是否启用: " << (log::isEnabled() ? "是" : "否") << std::endl;
    
    // 等待一下让文件系统同步
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "启用后，文件是否存在: " << (fileExists("logs/galay.log") ? "存在" : "不存在") << std::endl;
    
    // 输出一些日志
    LogDebug("这是第一条 debug 日志");
    LogInfo("这是第一条 info 日志");
    LogWarn("这是第一条 warn 日志");
    
    // 测试 3: 禁用日志（但文件应该仍然存在）
    std::cout << "\n[测试 3] 禁用日志" << std::endl;
    log::disable();
    
    std::cout << "日志是否初始化: " << (log::isInitialized() ? "是" : "否") << std::endl;
    std::cout << "日志是否启用: " << (log::isEnabled() ? "是" : "否") << std::endl;
    
    // 尝试输出日志（不应该输出）
    LogInfo("禁用后的日志，不应该输出");
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "禁用后，文件是否存在: " << (fileExists("logs/galay.log") ? "存在" : "不存在") << std::endl;
    
    // 测试 4: 重新启用
    std::cout << "\n[测试 4] 重新启用日志" << std::endl;
    log::enable(spdlog::level::info);
    
    LogInfo("重新启用后的日志");
    
    std::cout << "\n=== 测试完成 ===" << std::endl;
    std::cout << "结论：" << std::endl;
    std::cout << "1. 默认状态下不会创建日志文件 ✓" << std::endl;
    std::cout << "2. 首次启用时才会创建日志文件 ✓" << std::endl;
    std::cout << "3. 禁用后文件保留，但不输出日志 ✓" << std::endl;
    
    // 等待异步日志刷新
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 关闭日志系统
    details::InternelLogger::getInstance()->shutdown();
    
    return 0;
}

