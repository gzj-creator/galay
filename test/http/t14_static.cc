/**
 * @file test_static_file_transfer_modes.cc
 * @brief 测试静态文件的不同传输模式（MEMORY、CHUNK、SENDFILE）
 */

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include "galay-http/server/http_router.h"
#include "galay-http/server/file_settings.h"

using namespace galay::http;
namespace fs = std::filesystem;

// 创建测试文件
void createTestFiles(const std::string& baseDir) {
    fs::create_directories(baseDir);

    // 小文件 (10KB)
    std::ofstream small(baseDir + "/small.txt");
    for (int i = 0; i < 10 * 1024; i++) small.put('A');
    small.close();

    // 中等文件 (100KB)
    std::ofstream medium(baseDir + "/medium.txt");
    for (int i = 0; i < 100 * 1024; i++) medium.put('B');
    medium.close();

    // 大文件 (2MB)
    std::ofstream large(baseDir + "/large.txt");
    for (int i = 0; i < 2 * 1024 * 1024; i++) large.put('C');
    large.close();
}

void cleanupTestFiles(const std::string& baseDir) {
    if (fs::exists(baseDir)) {
        fs::remove_all(baseDir);
    }
}

// 测试 1: MEMORY 模式
void test_memory_mode() {
    std::cout << "\n=== Test 1: MEMORY Transfer Mode ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_memory_mode";
    createTestFiles(testDir);

    // 配置为 MEMORY 模式
    StaticFileSetting config;
    config.setTransferMode(FileTransferMode::MEMORY);

    router.mount("/memory", testDir, config);

    // 验证路由注册
    auto match = router.findHandler(HttpMethod::GET, "/memory/small.txt");
    assert(match.handler != nullptr);
    std::cout << "✓ MEMORY mode route registered" << std::endl;

    // 验证配置
    assert(config.getTransferMode() == FileTransferMode::MEMORY);
    std::cout << "✓ Transfer mode is MEMORY" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 1 passed!" << std::endl;
}

// 测试 2: CHUNK 模式
void test_chunk_mode() {
    std::cout << "\n=== Test 2: CHUNK Transfer Mode ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_chunk_mode";
    createTestFiles(testDir);

    // 配置为 CHUNK 模式
    StaticFileSetting config;
    config.setTransferMode(FileTransferMode::CHUNK);
    config.setChunkSize(32 * 1024);  // 32KB chunks

    router.mount("/chunk", testDir, config);

    auto match = router.findHandler(HttpMethod::GET, "/chunk/medium.txt");
    assert(match.handler != nullptr);
    std::cout << "✓ CHUNK mode route registered" << std::endl;

    assert(config.getTransferMode() == FileTransferMode::CHUNK);
    assert(config.getChunkSize() == 32 * 1024);
    std::cout << "✓ Transfer mode is CHUNK with 32KB chunks" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 2 passed!" << std::endl;
}

// 测试 3: SENDFILE 模式
void test_sendfile_mode() {
    std::cout << "\n=== Test 3: SENDFILE Transfer Mode ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_sendfile_mode";
    createTestFiles(testDir);

    // 配置为 SENDFILE 模式
    StaticFileSetting config;
    config.setTransferMode(FileTransferMode::SENDFILE);
    config.setSendFileChunkSize(1024 * 1024);  // 1MB per sendfile call

    router.mount("/sendfile", testDir, config);

    auto match = router.findHandler(HttpMethod::GET, "/sendfile/large.txt");
    assert(match.handler != nullptr);
    std::cout << "✓ SENDFILE mode route registered" << std::endl;

    assert(config.getTransferMode() == FileTransferMode::SENDFILE);
    assert(config.getSendFileChunkSize() == 1024 * 1024);
    std::cout << "✓ Transfer mode is SENDFILE with 1MB chunks" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 3 passed!" << std::endl;
}

// 测试 4: AUTO 模式（根据文件大小自动选择）
void test_auto_mode() {
    std::cout << "\n=== Test 4: AUTO Transfer Mode ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_auto_mode";
    createTestFiles(testDir);

    // 配置为 AUTO 模式
    StaticFileSetting config;
    config.setTransferMode(FileTransferMode::AUTO);
    config.setSmallFileThreshold(64 * 1024);   // 64KB
    config.setLargeFileThreshold(1024 * 1024); // 1MB

    router.mount("/auto", testDir, config);

    auto match = router.findHandler(HttpMethod::GET, "/auto/small.txt");
    assert(match.handler != nullptr);
    std::cout << "✓ AUTO mode route registered" << std::endl;

    // 测试自动选择逻辑
    size_t smallSize = 10 * 1024;   // 10KB
    size_t mediumSize = 100 * 1024; // 100KB
    size_t largeSize = 2 * 1024 * 1024; // 2MB

    assert(config.decideTransferMode(smallSize) == FileTransferMode::MEMORY);
    std::cout << "✓ Small file (10KB) -> MEMORY mode" << std::endl;

    assert(config.decideTransferMode(mediumSize) == FileTransferMode::CHUNK);
    std::cout << "✓ Medium file (100KB) -> CHUNK mode" << std::endl;

    assert(config.decideTransferMode(largeSize) == FileTransferMode::SENDFILE);
    std::cout << "✓ Large file (2MB) -> SENDFILE mode" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 4 passed!" << std::endl;
}

// 测试 5: mountHardly 使用不同传输模式
void test_mountHardly_with_modes() {
    std::cout << "\n=== Test 5: mountHardly with Different Modes ===" << std::endl;

    std::string testDir = "./test_mountHardly_modes";
    createTestFiles(testDir);

    // 测试 MEMORY 模式
    {
        HttpRouter router;
        StaticFileSetting config;
        config.setTransferMode(FileTransferMode::MEMORY);
        router.mountHardly("/static1", testDir, config);
        assert(router.size() >= 3);
        std::cout << "✓ mountHardly with MEMORY mode works" << std::endl;
    }

    // 测试 SENDFILE 模式
    {
        HttpRouter router;
        StaticFileSetting config;
        config.setTransferMode(FileTransferMode::SENDFILE);
        router.mountHardly("/static2", testDir, config);
        assert(router.size() >= 3);
        std::cout << "✓ mountHardly with SENDFILE mode works" << std::endl;
    }

    // 测试 AUTO 模式
    {
        HttpRouter router;
        StaticFileSetting config;
        config.setTransferMode(FileTransferMode::AUTO);
        router.mountHardly("/static3", testDir, config);
        assert(router.size() >= 3);
        std::cout << "✓ mountHardly with AUTO mode works" << std::endl;
    }

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 5 passed!" << std::endl;
}

// 测试 6: 配置参数验证
void test_config_parameters() {
    std::cout << "\n=== Test 6: Configuration Parameters ===" << std::endl;

    StaticFileSetting config;

    // 测试默认值
    assert(config.getTransferMode() == FileTransferMode::AUTO);
    assert(config.getSmallFileThreshold() == 64 * 1024);
    assert(config.getLargeFileThreshold() == 1024 * 1024);
    assert(config.getChunkSize() == 64 * 1024);
    assert(config.getSendFileChunkSize() == 10 * 1024 * 1024);
    std::cout << "✓ Default configuration values are correct" << std::endl;

    // 测试自定义配置
    config.setTransferMode(FileTransferMode::CHUNK);
    config.setSmallFileThreshold(32 * 1024);
    config.setLargeFileThreshold(512 * 1024);
    config.setChunkSize(16 * 1024);
    config.setSendFileChunkSize(5 * 1024 * 1024);

    assert(config.getTransferMode() == FileTransferMode::CHUNK);
    assert(config.getSmallFileThreshold() == 32 * 1024);
    assert(config.getLargeFileThreshold() == 512 * 1024);
    assert(config.getChunkSize() == 16 * 1024);
    assert(config.getSendFileChunkSize() == 5 * 1024 * 1024);
    std::cout << "✓ Custom configuration values work correctly" << std::endl;

    std::cout << "✓ Test 6 passed!" << std::endl;
}

// 测试 7: 向后兼容性（不提供配置参数）
void test_backward_compatibility() {
    std::cout << "\n=== Test 7: Backward Compatibility ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_backward_compat";
    createTestFiles(testDir);

    // 不提供配置参数，应该使用默认配置（AUTO 模式）
    router.mount("/default", testDir);

    auto match = router.findHandler(HttpMethod::GET, "/default/small.txt");
    assert(match.handler != nullptr);
    std::cout << "✓ mount() without config parameter works (backward compatible)" << std::endl;

    router.mountHardly("/default2", testDir);
    assert(router.size() >= 4);
    std::cout << "✓ mountHardly() without config parameter works (backward compatible)" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 7 passed!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Static File Transfer Modes Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        test_memory_mode();
        test_chunk_mode();
        test_sendfile_mode();
        test_auto_mode();
        test_mountHardly_with_modes();
        test_config_parameters();
        test_backward_compatibility();

        std::cout << "\n========================================" << std::endl;
        std::cout << "All tests passed! ✓" << std::endl;
        std::cout << "========================================" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n✗ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
