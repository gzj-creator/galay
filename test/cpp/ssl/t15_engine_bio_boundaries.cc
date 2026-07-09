/**
 * @file t15_engine_bio_boundaries.cc
 * @brief 覆盖 SslEngine Memory BIO 边界错误必须显式返回。
 */

#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-ssl/ssl/ssl_engine.h>

#include <climits>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace galay::ssl;

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    SslContext ctx(SslMethod::TLS_Client);
    if (!expect(ctx.isValid(), "ssl context invalid")) {
        return 1;
    }

    SslEngine engine(&ctx);
    if (!expect(engine.isValid(), "ssl engine invalid")) {
        return 1;
    }

    std::expected<size_t, SslError> uninitialized_feed = engine.feedEncryptedInput("x", 1);
    if (!expect(!uninitialized_feed, "feed without rbio should fail")) {
        return 1;
    }
    if (!expect(uninitialized_feed.error().code() == SslErrorCode::kReadFailed,
                "feed without rbio returned unexpected error")) {
        return 1;
    }

    char out = '\0';
    std::expected<size_t, SslError> uninitialized_extract = engine.extractEncryptedOutput(&out, 1);
    if (!expect(!uninitialized_extract, "extract without wbio should fail")) {
        return 1;
    }
    if (!expect(uninitialized_extract.error().code() == SslErrorCode::kWriteFailed,
                "extract without wbio returned unexpected error")) {
        return 1;
    }

    if (!expect(engine.initMemoryBIO().has_value(), "initMemoryBIO failed")) {
        return 1;
    }

    std::expected<size_t, SslError> oversized_feed =
        engine.feedEncryptedInput("x", static_cast<size_t>(INT_MAX) + 1U);
    if (!expect(!oversized_feed, "oversized feed should fail")) {
        return 1;
    }
    if (!expect(oversized_feed.error().code() == SslErrorCode::kBufferTooLarge,
                "oversized feed returned unexpected error")) {
        return 1;
    }

    std::expected<size_t, SslError> oversized_extract =
        engine.extractEncryptedOutput(&out, static_cast<size_t>(INT_MAX) + 1U);
    if (!expect(!oversized_extract, "oversized extract should fail")) {
        return 1;
    }
    if (!expect(oversized_extract.error().code() == SslErrorCode::kBufferTooLarge,
                "oversized extract returned unexpected error")) {
        return 1;
    }

    const std::filesystem::path source_path =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
        "src/cpp/galay-ssl/async/ssl_await.h";
    std::ifstream source(source_path);
    if (!expect(source.good(), "failed to open ssl_await.h")) {
        return 1;
    }
    const std::string source_text((std::istreambuf_iterator<char>(source)),
                                  std::istreambuf_iterator<char>());
    if (!expect(source_text.find("std::abort()") == std::string::npos,
                "SSL awaitable error propagation must not abort the process")) {
        return 1;
    }

    return 0;
}
