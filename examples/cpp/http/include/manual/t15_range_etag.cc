/**
 * @file T15-RangeEtagServer.cc
 * @brief Range 和 ETag 测试服务器
 * @details 提供静态文件服务，支持 Range 请求和 ETag 验证，配合 HTML 前端测试
 */

#include <galay/cpp/galay-http/server/http_server.h>
#include <galay/cpp/galay-http/server/http_router.h>
#include <galay/cpp/galay-http/kernel/http_conn.h>
#include <galay/cpp/galay-http/server/file_settings.h>
#include <galay/cpp/galay-http/server/http_etag.h>
#include <galay/cpp/galay-http/server/http_range.h>
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <iostream>
#include <filesystem>
#include <fstream>

using namespace galay::http;
using namespace galay::kernel;
namespace fs = std::filesystem;

// CORS 处理器 - 处理 OPTIONS 预检请求
Task<void> corsHandler(HttpConn& conn, HttpRequest req) {
    std::cout << "CORS OPTIONS request received for: " << req.header().uri() << std::endl;

    auto response = Http1_1ResponseBuilder::noContent()
        .header("Access-Control-Allow-Origin", "*")
        .header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
        .header("Access-Control-Allow-Headers", "Content-Type, Range, If-None-Match, If-Match, If-Range, Cache-Control, Pragma")
        .header("Access-Control-Expose-Headers", "Content-Range, ETag, Content-Length, Accept-Ranges")
        .header("Access-Control-Max-Age", "86400")
        .build();

    auto writer = conn.getWriter();
    auto send_result = co_await writer.sendResponse(response);
    if (!send_result) {
        std::cout << "  -> Send error: " << send_result.error().message() << std::endl;
        co_return;
    }

    std::cout << "  -> 204 No Content (CORS preflight)" << std::endl;
    co_return;
}

// 自定义文件处理器 - 添加 CORS 头
Task<void> fileHandlerWithCORS(HttpConn& conn, HttpRequest req) {
    // 获取请求的文件路径
    std::string uri = req.header().uri();

    // 移除 /files 前缀
    std::string filePath = "./files" + uri.substr(6);  // 去掉 "/files"

    std::cout << "Request: " << httpMethodToString(req.header().method()) << " " << uri << std::endl;
    std::cout << "File path: " << filePath << std::endl;

    // 打印请求头（用于调试）
    std::string ifNoneMatchHeader = req.header().headerPairs().getValue("If-None-Match");
    if (!ifNoneMatchHeader.empty()) {
        std::cout << "  If-None-Match: " << ifNoneMatchHeader << std::endl;
    }

    // 检查文件是否存在
    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        auto response = Http1_1ResponseBuilder::notFound()
            .header("Access-Control-Allow-Origin", "*")
            .text("File not found")
            .build();

        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(response);
        if (!send_result) {
            std::cout << "  -> Send error: " << send_result.error().message() << std::endl;
            co_return;
        }

        // 不要手动关闭连接，让服务器自动管理
        co_return;
    }

    // 获取文件大小
    size_t fileSize = fs::file_size(filePath);

    // 生成 ETag
    std::string etag = ETagGenerator::generate(filePath);

    // 检查 If-None-Match
    std::string ifNoneMatch = req.header().headerPairs().getValue("If-None-Match");
    if (!ifNoneMatch.empty() && ETagGenerator::match(etag, ifNoneMatch)) {
        std::cout << "  -> 304 Not Modified (ETag match)" << std::endl;

        auto response = Http1_1ResponseBuilder()
            .status(304)
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Expose-Headers", "ETag")
            .header("ETag", etag)
            .build();

        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(response);
        if (!send_result) {
            std::cout << "  -> Send error: " << send_result.error().message() << std::endl;
            co_return;
        }

        // 不要手动关闭连接，让服务器自动管理
        co_return;
    }

    // 检查 Range 请求
    std::string rangeHeader = req.header().headerPairs().getValue("Range");

    if (!rangeHeader.empty()) {
        std::cout << "  Range: " << rangeHeader << std::endl;

        // 检查 If-Range
        std::string ifRange = req.header().headerPairs().getValue("If-Range");
        if (!ifRange.empty() && !ETagGenerator::match(etag, ifRange)) {
            std::cout << "  -> If-Range mismatch, returning full file" << std::endl;
            rangeHeader.clear();  // 忽略 Range，返回完整文件
        }
    }

    // 读取文件
    std::ifstream file(filePath, std::ios::binary);

    if (rangeHeader.empty()) {
        // 返回完整文件
        std::cout << "  -> 200 OK (full file, " << fileSize << " bytes)" << std::endl;

        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
        file.close();

        auto response = Http1_1ResponseBuilder::ok()
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Expose-Headers", "Content-Range, ETag, Content-Length, Accept-Ranges")
            .header("ETag", etag)
            .header("Accept-Ranges", "bytes")
            .header("Content-Type", "application/octet-stream")
            .body(content)
            .build();

        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(response);
        if (!send_result) {
            std::cout << "  -> Send error: " << send_result.error().message() << std::endl;
            co_return;
        }

    } else {
        // 解析 Range
        auto rangeResult = HttpRangeParser::parse(rangeHeader, fileSize);

        if (!rangeResult.isValid() || rangeResult.ranges.empty()) {
            std::cout << "  -> 416 Range Not Satisfiable" << std::endl;

            auto response = Http1_1ResponseBuilder()
                .status(416)
                .header("Access-Control-Allow-Origin", "*")
                .header("Content-Range", "bytes */" + std::to_string(fileSize))
                .build();

            auto writer = conn.getWriter();
            auto send_result = co_await writer.sendResponse(response);
            if (!send_result) {
                std::cout << "  -> Send error: " << send_result.error().message() << std::endl;
                co_return;
            }

            co_return;
        }

        // 只处理单范围请求
        const auto& range = rangeResult.ranges[0];
        std::cout << "  -> 206 Partial Content (bytes " << range.start << "-" << range.end << "/" << fileSize << ")" << std::endl;

        // 读取指定范围
        file.seekg(range.start);
        std::vector<char> buffer(range.length);
        file.read(buffer.data(), range.length);
        file.close();

        std::string content(buffer.begin(), buffer.end());

        auto response = Http1_1ResponseBuilder()
            .status(206)
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Expose-Headers", "Content-Range, ETag, Content-Length, Accept-Ranges")
            .header("ETag", etag)
            .header("Accept-Ranges", "bytes")
            .header("Content-Range", HttpRangeParser::makeContentRange(range, fileSize))
            .header("Content-Type", "application/octet-stream")
            .body(content)
            .build();

        auto writer = conn.getWriter();
        auto send_result = co_await writer.sendResponse(response);
        if (!send_result) {
            std::cout << "  -> Send error: " << send_result.error().message() << std::endl;
            co_return;
        }
    }

    // 不要手动关闭连接，让服务器自动管理
    co_return;
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "========================================\n";
    std::cout << "Range & ETag Test Server\n";
    std::cout << "========================================\n";

    try {
        // 创建测试文件目录
        fs::create_directories("./files");

        // 创建测试文件（如果不存在）
        auto create_test_file = [](const std::string& path, size_t size) {
            if (!fs::exists(path)) {
                std::cout << "Creating test file: " << path << " (" << size / 1024 / 1024 << " MB)\n";
                std::ofstream file(path, std::ios::binary);
                std::vector<char> data(size);
                for (size_t i = 0; i < size; ++i) {
                    data[i] = static_cast<char>(i % 256);
                }
                file.write(data.data(), size);
                file.close();
            }
        };

        create_test_file("./files/test_small.bin", 1 * 1024 * 1024);      // 1 MB
        create_test_file("./files/test_medium.bin", 5 * 1024 * 1024);     // 5 MB
        create_test_file("./files/test_large.bin", 10 * 1024 * 1024);     // 10 MB

        // 创建路由器
        HttpRouter router;

        // 添加 CORS 预检请求处理（OPTIONS 方法）
        router.addHandler<HttpMethod::OPTIONS>("/files/*", corsHandler);

        // 添加文件处理器（带 CORS 支持）
        router.addHandler<HttpMethod::GET>("/files/*", fileHandlerWithCORS);

        // 创建并启动服务器
        HttpServer server(HttpServerBuilder()
            .host("0.0.0.0")
            .port(port)
            .build());

        std::cout << "========================================\n";
        std::cout << "Server is running on http://0.0.0.0:" << port << "\n";
        std::cout << "========================================\n";
        std::cout << "Test Files (API Endpoints):\n";
        std::cout << "  - http://localhost:" << port << "/files/test_small.bin  (1 MB)\n";
        std::cout << "  - http://localhost:" << port << "/files/test_medium.bin (5 MB)\n";
        std::cout << "  - http://localhost:" << port << "/files/test_large.bin  (10 MB)\n";
        std::cout << "========================================\n";
        std::cout << "How to Test:\n";
        std::cout << "  1. Open html/ResumeDownload.html in browser\n";
        std::cout << "  2. Open html/EtagCache.html in browser\n";
        std::cout << "  3. Click buttons to test Range and ETag features\n";
        std::cout << "========================================\n";
        std::cout << "Features:\n";
        std::cout << "  ✓ Range requests support\n";
        std::cout << "  ✓ ETag validation (If-None-Match, If-Range)\n";
        std::cout << "  ✓ 304 Not Modified responses\n";
        std::cout << "  ✓ Auto transfer mode (MEMORY/CHUNK/SENDFILE)\n";
        std::cout << "========================================\n";
        std::cout << "Press Ctrl+C to stop the server\n";
        std::cout << "========================================\n\n";

        server.start(std::move(router));

        // 保持服务器运行
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
