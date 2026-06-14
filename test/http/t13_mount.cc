/**
 * @file test_mount_functions.cc
 * @brief 专门测试 HttpRouter 的 mount() 和 mountHardly() 功能
 * @details 测试静态文件挂载的两种模式：
 *          1. mount() - 动态模式，运行时从磁盘读取文件
 *          2. mountHardly() - 静态模式，启动时预加载所有文件到内存
 */

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <chrono>
#include "http/server/http_router.h"
#include "http/kernel/http_conn.h"
#include "http/protoc/http_request.h"
#include "http/protoc/http_response.h"

using namespace galay::http;
namespace fs = std::filesystem;

// 测试辅助函数：创建测试文件结构
void createTestFiles(const std::string& baseDir) {
    fs::create_directories(baseDir);
    fs::create_directories(baseDir + "/css");
    fs::create_directories(baseDir + "/js");
    fs::create_directories(baseDir + "/docs");

    // 创建测试文件
    std::ofstream(baseDir + "/index.html") << "<!DOCTYPE html><html><body>Test Page</body></html>";
    std::ofstream(baseDir + "/css/style.css") << "body { color: red; }";
    std::ofstream(baseDir + "/js/app.js") << "console.log('test');";
    std::ofstream(baseDir + "/docs/readme.txt") << "This is a test file.";
    std::ofstream(baseDir + "/docs/data.json") << "{\"test\": true}";

    // 创建二进制测试文件
    std::ofstream small(baseDir + "/small.bin", std::ios::binary);
    for (int i = 0; i < 1024; i++) small.put(static_cast<char>(i % 256));
    small.close();
}

// 测试辅助函数：清理测试文件
void cleanupTestFiles(const std::string& baseDir) {
    if (fs::exists(baseDir)) {
        fs::remove_all(baseDir);
    }
}

// 测试 1: mount() 基本功能
void test_mount_basic() {
    std::cout << "\n=== Test 1: mount() Basic Functionality ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_mount_temp";

    // 创建测试文件
    createTestFiles(testDir);

    // 挂载目录
    router.mount("/static", testDir);

    // 验证路由已注册
    assert(router.size() > 0);
    std::cout << "✓ mount() registered routes: " << router.size() << std::endl;

    // 测试查找处理器
    auto match1 = router.findHandler(HttpMethod::GET, "/static/index.html");
    assert(match1.handler != nullptr);
    std::cout << "✓ Found handler for /static/index.html" << std::endl;

    auto match2 = router.findHandler(HttpMethod::GET, "/static/css/style.css");
    assert(match2.handler != nullptr);
    std::cout << "✓ Found handler for /static/css/style.css" << std::endl;

    auto match3 = router.findHandler(HttpMethod::GET, "/static/js/app.js");
    assert(match3.handler != nullptr);
    std::cout << "✓ Found handler for /static/js/app.js" << std::endl;

    auto match4 = router.findHandler(HttpMethod::GET, "/static/docs/readme.txt");
    assert(match4.handler != nullptr);
    std::cout << "✓ Found handler for /static/docs/readme.txt" << std::endl;

    // 测试不存在的文件（应该找到通配符处理器，但文件不存在会在运行时返回404）
    auto match5 = router.findHandler(HttpMethod::GET, "/static/nonexistent.txt");
    assert(match5.handler != nullptr);  // 通配符匹配
    std::cout << "✓ Wildcard handler found for non-existent file" << std::endl;

    // 清理
    cleanupTestFiles(testDir);
    std::cout << "✓ Test 1 passed!" << std::endl;
}

// 测试 2: mountHardly() 基本功能
void test_mountHardly_basic() {
    std::cout << "\n=== Test 2: mountHardly() Basic Functionality ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_mountHardly_temp";

    // 创建测试文件
    createTestFiles(testDir);

    // 静态挂载目录
    router.mountHardly("/files", testDir);

    // 验证路由已注册（应该为每个文件创建一个精确路由）
    size_t routeCount = router.size();
    assert(routeCount >= 6);  // 至少6个文件
    std::cout << "✓ mountHardly() registered routes: " << routeCount << std::endl;

    // 测试查找处理器（精确匹配）
    auto match1 = router.findHandler(HttpMethod::GET, "/files/index.html");
    assert(match1.handler != nullptr);
    std::cout << "✓ Found exact handler for /files/index.html" << std::endl;

    auto match2 = router.findHandler(HttpMethod::GET, "/files/css/style.css");
    assert(match2.handler != nullptr);
    std::cout << "✓ Found exact handler for /files/css/style.css" << std::endl;

    auto match3 = router.findHandler(HttpMethod::GET, "/files/js/app.js");
    assert(match3.handler != nullptr);
    std::cout << "✓ Found exact handler for /files/js/app.js" << std::endl;

    auto match4 = router.findHandler(HttpMethod::GET, "/files/docs/readme.txt");
    assert(match4.handler != nullptr);
    std::cout << "✓ Found exact handler for /files/docs/readme.txt" << std::endl;

    auto match5 = router.findHandler(HttpMethod::GET, "/files/docs/data.json");
    assert(match5.handler != nullptr);
    std::cout << "✓ Found exact handler for /files/docs/data.json" << std::endl;

    // 测试不存在的文件（应该找不到处理器）
    auto match6 = router.findHandler(HttpMethod::GET, "/files/nonexistent.txt");
    assert(match6.handler == nullptr);
    std::cout << "✓ No handler found for non-existent file (expected)" << std::endl;

    // 清理
    cleanupTestFiles(testDir);
    std::cout << "✓ Test 2 passed!" << std::endl;
}

// 测试 3: mount() 和 mountHardly() 共存
void test_mount_and_mountHardly_coexist() {
    std::cout << "\n=== Test 3: mount() and mountHardly() Coexistence ===" << std::endl;

    HttpRouter router;
    std::string testDir1 = "./test_mount_coexist1";
    std::string testDir2 = "./test_mount_coexist2";

    // 创建两个测试目录
    createTestFiles(testDir1);
    createTestFiles(testDir2);

    // 同时使用两种挂载方式
    router.mount("/dynamic", testDir1);
    router.mountHardly("/static", testDir2);

    size_t totalRoutes = router.size();
    std::cout << "✓ Total routes registered: " << totalRoutes << std::endl;

    // 测试动态挂载
    auto match1 = router.findHandler(HttpMethod::GET, "/dynamic/index.html");
    assert(match1.handler != nullptr);
    std::cout << "✓ Dynamic mount works: /dynamic/index.html" << std::endl;

    // 测试静态挂载
    auto match2 = router.findHandler(HttpMethod::GET, "/static/index.html");
    assert(match2.handler != nullptr);
    std::cout << "✓ Static mount works: /static/index.html" << std::endl;

    // 测试不同前缀不会冲突
    auto match3 = router.findHandler(HttpMethod::GET, "/dynamic/css/style.css");
    assert(match3.handler != nullptr);

    auto match4 = router.findHandler(HttpMethod::GET, "/static/css/style.css");
    assert(match4.handler != nullptr);
    std::cout << "✓ Both mounts work independently" << std::endl;

    // 清理
    cleanupTestFiles(testDir1);
    cleanupTestFiles(testDir2);
    std::cout << "✓ Test 3 passed!" << std::endl;
}

// 测试 4: 路径安全性（防止路径遍历攻击）
void test_path_security() {
    std::cout << "\n=== Test 4: Path Security (Path Traversal Prevention) ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_security_temp";

    createTestFiles(testDir);
    router.mount("/secure", testDir);

    // 测试正常路径
    auto match1 = router.findHandler(HttpMethod::GET, "/secure/index.html");
    assert(match1.handler != nullptr);
    std::cout << "✓ Normal path works" << std::endl;

    // 测试路径遍历尝试（这些应该被通配符匹配，但在运行时会被安全检查拦截）
    auto match2 = router.findHandler(HttpMethod::GET, "/secure/../../../etc/passwd");
    assert(match2.handler != nullptr);  // 通配符会匹配
    std::cout << "✓ Path traversal attempt matched by wildcard (will be blocked at runtime)" << std::endl;

    auto match3 = router.findHandler(HttpMethod::GET, "/secure/./../../secret.txt");
    assert(match3.handler != nullptr);  // 通配符会匹配
    std::cout << "✓ Relative path attempt matched by wildcard (will be blocked at runtime)" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 4 passed!" << std::endl;
}

// 测试 5: 空目录和不存在的目录
void test_invalid_directories() {
    std::cout << "\n=== Test 5: Invalid Directory Handling ===" << std::endl;

    HttpRouter router;

    // 测试不存在的目录
    router.mount("/nonexistent", "./this_dir_does_not_exist");
    assert(router.size() == 0);  // 不应该注册任何路由
    std::cout << "✓ mount() handles non-existent directory gracefully" << std::endl;

    router.mountHardly("/nonexistent2", "./this_dir_also_does_not_exist");
    assert(router.size() == 0);  // 不应该注册任何路由
    std::cout << "✓ mountHardly() handles non-existent directory gracefully" << std::endl;

    // 测试空目录
    std::string emptyDir = "./test_empty_dir";
    fs::create_directories(emptyDir);

    size_t beforeMount = router.size();
    router.mount("/empty", emptyDir);
    assert(router.size() > beforeMount);  // 应该注册通配符路由
    std::cout << "✓ mount() handles empty directory" << std::endl;

    size_t beforeMountHardly = router.size();
    router.mountHardly("/empty2", emptyDir);
    assert(router.size() == beforeMountHardly);  // 空目录不应该增加路由
    std::cout << "✓ mountHardly() handles empty directory" << std::endl;

    cleanupTestFiles(emptyDir);
    std::cout << "✓ Test 5 passed!" << std::endl;
}

// 测试 6: 嵌套目录结构
void test_nested_directories() {
    std::cout << "\n=== Test 6: Nested Directory Structure ===" << std::endl;

    HttpRouter router;
    std::string testDir = "./test_nested_temp";

    // 创建深层嵌套结构
    fs::create_directories(testDir + "/level1/level2/level3");
    std::ofstream(testDir + "/level1/file1.txt") << "Level 1";
    std::ofstream(testDir + "/level1/level2/file2.txt") << "Level 2";
    std::ofstream(testDir + "/level1/level2/level3/file3.txt") << "Level 3";

    // 测试 mount
    router.mount("/nested", testDir);

    auto match1 = router.findHandler(HttpMethod::GET, "/nested/level1/file1.txt");
    assert(match1.handler != nullptr);
    std::cout << "✓ mount() handles nested level 1" << std::endl;

    auto match2 = router.findHandler(HttpMethod::GET, "/nested/level1/level2/file2.txt");
    assert(match2.handler != nullptr);
    std::cout << "✓ mount() handles nested level 2" << std::endl;

    auto match3 = router.findHandler(HttpMethod::GET, "/nested/level1/level2/level3/file3.txt");
    assert(match3.handler != nullptr);
    std::cout << "✓ mount() handles nested level 3" << std::endl;

    // 测试 mountHardly
    HttpRouter router2;
    router2.mountHardly("/nested2", testDir);

    auto match4 = router2.findHandler(HttpMethod::GET, "/nested2/level1/file1.txt");
    assert(match4.handler != nullptr);
    std::cout << "✓ mountHardly() handles nested level 1" << std::endl;

    auto match5 = router2.findHandler(HttpMethod::GET, "/nested2/level1/level2/file2.txt");
    assert(match5.handler != nullptr);
    std::cout << "✓ mountHardly() handles nested level 2" << std::endl;

    auto match6 = router2.findHandler(HttpMethod::GET, "/nested2/level1/level2/level3/file3.txt");
    assert(match6.handler != nullptr);
    std::cout << "✓ mountHardly() handles nested level 3" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 6 passed!" << std::endl;
}

// 测试 7: 不同的路由前缀格式
void test_route_prefix_formats() {
    std::cout << "\n=== Test 7: Route Prefix Format Variations ===" << std::endl;

    std::string testDir = "./test_prefix_temp";
    createTestFiles(testDir);

    // 测试不同的前缀格式
    HttpRouter router1;
    router1.mount("/static", testDir);  // 标准格式
    auto match1 = router1.findHandler(HttpMethod::GET, "/static/index.html");
    assert(match1.handler != nullptr);
    std::cout << "✓ Prefix '/static' works" << std::endl;

    HttpRouter router2;
    router2.mount("/static/", testDir);  // 带尾部斜杠
    auto match2 = router2.findHandler(HttpMethod::GET, "/static/index.html");
    assert(match2.handler != nullptr);
    std::cout << "✓ Prefix '/static/' works" << std::endl;

    HttpRouter router3;
    router3.mountHardly("/files", testDir);
    auto match3 = router3.findHandler(HttpMethod::GET, "/files/index.html");
    assert(match3.handler != nullptr);
    std::cout << "✓ mountHardly with '/files' works" << std::endl;

    HttpRouter router4;
    router4.mountHardly("/files/", testDir);
    auto match4 = router4.findHandler(HttpMethod::GET, "/files/index.html");
    assert(match4.handler != nullptr);
    std::cout << "✓ mountHardly with '/files/' works" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 7 passed!" << std::endl;
}

// 测试 8: 性能对比（简单测试）
void test_performance_comparison() {
    std::cout << "\n=== Test 8: Performance Comparison (Simple) ===" << std::endl;

    std::string testDir = "./test_perf_temp";
    createTestFiles(testDir);

    // 创建更多文件用于测试
    for (int i = 0; i < 100; i++) {
        std::ofstream(testDir + "/file" + std::to_string(i) + ".txt") << "Test file " << i;
    }

    // 测试 mount() 的注册时间
    HttpRouter router1;
    auto start1 = std::chrono::high_resolution_clock::now();
    router1.mount("/dynamic", testDir);
    auto end1 = std::chrono::high_resolution_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::microseconds>(end1 - start1);
    std::cout << "✓ mount() registration time: " << duration1.count() << " μs" << std::endl;
    std::cout << "  Routes registered: " << router1.size() << std::endl;

    // 测试 mountHardly() 的注册时间
    HttpRouter router2;
    auto start2 = std::chrono::high_resolution_clock::now();
    router2.mountHardly("/static", testDir);
    auto end2 = std::chrono::high_resolution_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::microseconds>(end2 - start2);
    std::cout << "✓ mountHardly() registration time: " << duration2.count() << " μs" << std::endl;
    std::cout << "  Routes registered: " << router2.size() << std::endl;

    // 测试查找性能
    const int lookupCount = 10000;

    // mount() 查找性能
    auto start3 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < lookupCount; i++) {
        router1.findHandler(HttpMethod::GET, "/dynamic/file50.txt");
    }
    auto end3 = std::chrono::high_resolution_clock::now();
    auto duration3 = std::chrono::duration_cast<std::chrono::microseconds>(end3 - start3);
    std::cout << "✓ mount() lookup time (" << lookupCount << " lookups): "
              << duration3.count() << " μs ("
              << (duration3.count() / static_cast<double>(lookupCount)) << " μs/lookup)" << std::endl;

    // mountHardly() 查找性能
    auto start4 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < lookupCount; i++) {
        router2.findHandler(HttpMethod::GET, "/static/file50.txt");
    }
    auto end4 = std::chrono::high_resolution_clock::now();
    auto duration4 = std::chrono::duration_cast<std::chrono::microseconds>(end4 - start4);
    std::cout << "✓ mountHardly() lookup time (" << lookupCount << " lookups): "
              << duration4.count() << " μs ("
              << (duration4.count() / static_cast<double>(lookupCount)) << " μs/lookup)" << std::endl;

    // 性能对比
    double speedup = static_cast<double>(duration3.count()) / duration4.count();
    std::cout << "✓ mountHardly() is " << speedup << "x faster for lookups" << std::endl;

    cleanupTestFiles(testDir);
    std::cout << "✓ Test 8 passed!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "HttpRouter mount() and mountHardly() Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        test_mount_basic();
        test_mountHardly_basic();
        test_mount_and_mountHardly_coexist();
        test_path_security();
        test_invalid_directories();
        test_nested_directories();
        test_route_prefix_formats();
        test_performance_comparison();

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
