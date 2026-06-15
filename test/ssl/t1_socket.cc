/**
 * @file test_ssl_socket.cc
 * @brief SSL Socket 单元测试 (无 GTest 依赖)
 */

#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"
#include "galay-ssl/ssl/ssl_engine.h"
#include "galay-ssl/common/error.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace galay::ssl;

// 简单测试框架
static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cout << "Running: " #name << " ... "; \
            g_tests_run++; \
            try { \
                test_##name(); \
                std::cout << "PASSED" << std::endl; \
                g_tests_passed++; \
            } catch (const std::exception& e) { \
                std::cout << "FAILED: " << e.what() << std::endl; \
                g_tests_failed++; \
            } catch (...) { \
                std::cout << "FAILED: unknown exception" << std::endl; \
                g_tests_failed++; \
            } \
        } \
    } g_test_runner_##name; \
    void test_##name()

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error("EXPECT_TRUE failed: " #expr); \
        } \
    } while(0)

#define EXPECT_FALSE(expr) \
    do { \
        if (expr) { \
            throw std::runtime_error("EXPECT_FALSE failed: " #expr); \
        } \
    } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw std::runtime_error("EXPECT_EQ failed: " #a " != " #b); \
        } \
    } while(0)

#define EXPECT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            throw std::runtime_error("EXPECT_NE failed: " #a " == " #b); \
        } \
    } while(0)

// ==================== SslContext 测试 ====================

TEST(SslContext_CreateServerContext) {
    SslContext ctx(SslMethod::TLS_Server);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_CreateClientContext) {
    SslContext ctx(SslMethod::TLS_Client);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_CreateTLS12Server) {
    SslContext ctx(SslMethod::TLS_1_2_Server);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_CreateTLS12Client) {
    SslContext ctx(SslMethod::TLS_1_2_Client);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_CreateTLS13Server) {
    SslContext ctx(SslMethod::TLS_1_3_Server);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_CreateTLS13Client) {
    SslContext ctx(SslMethod::TLS_1_3_Client);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_LoadCertificateNotFound) {
    SslContext ctx(SslMethod::TLS_Server);
    auto result = ctx.loadCertificate("nonexistent.crt");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), SslErrorCode::kCertificateLoadFailed);
}

TEST(SslContext_LoadPrivateKeyNotFound) {
    SslContext ctx(SslMethod::TLS_Server);
    auto result = ctx.loadPrivateKey("nonexistent.key");
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), SslErrorCode::kPrivateKeyLoadFailed);
}

TEST(SslContext_LoadCertificateSuccess) {
    SslContext ctx(SslMethod::TLS_Server);
    auto result = ctx.loadCertificate("certs/server.crt");
    EXPECT_TRUE(result.has_value());
}

TEST(SslContext_LoadPrivateKeySuccess) {
    SslContext ctx(SslMethod::TLS_Server);
    ctx.loadCertificate("certs/server.crt");  // 先加载证书
    auto result = ctx.loadPrivateKey("certs/server.key");
    EXPECT_TRUE(result.has_value());
}

TEST(SslContext_LoadCertificateChain) {
    SslContext ctx(SslMethod::TLS_Server);
    auto result = ctx.loadCertificateChain("certs/server.crt");
    EXPECT_TRUE(result.has_value());
}

TEST(SslContext_LoadCACertificate) {
    SslContext ctx(SslMethod::TLS_Client);
    auto result = ctx.loadCACertificate("certs/ca.crt");
    EXPECT_TRUE(result.has_value());
}

TEST(SslContext_SetVerifyModeNone) {
    SslContext ctx(SslMethod::TLS_Client);
    ctx.setVerifyMode(SslVerifyMode::None);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_SetVerifyModePeer) {
    SslContext ctx(SslMethod::TLS_Client);
    ctx.setVerifyMode(SslVerifyMode::Peer);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_SetCiphers) {
    SslContext ctx(SslMethod::TLS_Server);
    auto result = ctx.setCiphers("HIGH:!aNULL:!MD5");
    EXPECT_TRUE(result.has_value());
}

TEST(SslContext_SetCiphersuites) {
    SslContext ctx(SslMethod::TLS_1_3_Server);
    auto result = ctx.setCiphersuites("TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
    EXPECT_TRUE(result.has_value());
}

TEST(SslContext_SetVerifyDepth) {
    SslContext ctx(SslMethod::TLS_Client);
    ctx.setVerifyDepth(4);
    EXPECT_TRUE(ctx.isValid());
}

TEST(SslContext_MoveConstruct) {
    SslContext ctx1(SslMethod::TLS_Server);
    EXPECT_TRUE(ctx1.isValid());

    SslContext ctx2(std::move(ctx1));
    EXPECT_TRUE(ctx2.isValid());
    EXPECT_FALSE(ctx1.isValid());
}

TEST(SslContext_MoveAssign) {
    SslContext ctx1(SslMethod::TLS_Server);
    SslContext ctx2(SslMethod::TLS_Client);
    EXPECT_TRUE(ctx1.isValid());
    EXPECT_TRUE(ctx2.isValid());

    ctx2 = std::move(ctx1);
    EXPECT_TRUE(ctx2.isValid());
    EXPECT_FALSE(ctx1.isValid());
}

// ==================== SslEngine 测试 ====================

TEST(SslEngine_CreateEngine) {
    SslContext ctx(SslMethod::TLS_Client);
    SslEngine engine(&ctx);
    EXPECT_TRUE(engine.isValid());
}

TEST(SslEngine_SetHostname) {
    SslContext ctx(SslMethod::TLS_Client);
    SslEngine engine(&ctx);
    auto result = engine.setHostname("example.com");
    EXPECT_TRUE(result.has_value());
}

TEST(SslEngine_SetConnectState) {
    SslContext ctx(SslMethod::TLS_Client);
    SslEngine engine(&ctx);
    engine.setConnectState();
    EXPECT_TRUE(engine.isValid());
}

TEST(SslEngine_SetAcceptState) {
    SslContext ctx(SslMethod::TLS_Server);
    SslEngine engine(&ctx);
    engine.setAcceptState();
    EXPECT_TRUE(engine.isValid());
}

TEST(SslEngine_MoveConstruct) {
    SslContext ctx(SslMethod::TLS_Client);
    SslEngine engine1(&ctx);
    EXPECT_TRUE(engine1.isValid());

    SslEngine engine2(std::move(engine1));
    EXPECT_TRUE(engine2.isValid());
    EXPECT_FALSE(engine1.isValid());
}

TEST(SslEngine_MoveAssign) {
    SslContext ctx(SslMethod::TLS_Client);
    SslEngine engine1(&ctx);
    SslEngine engine2(&ctx);
    EXPECT_TRUE(engine1.isValid());
    EXPECT_TRUE(engine2.isValid());

    engine2 = std::move(engine1);
    EXPECT_TRUE(engine2.isValid());
    EXPECT_FALSE(engine1.isValid());
}

TEST(SslEngine_HandshakeStateInitial) {
    SslContext ctx(SslMethod::TLS_Client);
    SslEngine engine(&ctx);
    EXPECT_EQ(engine.handshakeState(), SslHandshakeState::NotStarted);
}

// ==================== SslError 测试 ====================

TEST(SslError_SuccessError) {
    SslError err;
    EXPECT_TRUE(err.isSuccess());
    EXPECT_EQ(err.code(), SslErrorCode::kSuccess);
}

TEST(SslError_ErrorCode) {
    SslError err(SslErrorCode::kHandshakeFailed);
    EXPECT_FALSE(err.isSuccess());
    EXPECT_EQ(err.code(), SslErrorCode::kHandshakeFailed);
}

TEST(SslError_ErrorMessage) {
    SslError err(SslErrorCode::kHandshakeFailed);
    std::string msg = err.message();
    EXPECT_FALSE(msg.empty());
}

TEST(SslError_NeedsRetryWantRead) {
    SslError err(SslErrorCode::kHandshakeWantRead);
    EXPECT_TRUE(err.needsRetry());
}

TEST(SslError_NeedsRetryWantWrite) {
    SslError err(SslErrorCode::kHandshakeWantWrite);
    EXPECT_TRUE(err.needsRetry());
}

TEST(SslError_NeedsRetryFailed) {
    SslError err(SslErrorCode::kHandshakeFailed);
    EXPECT_FALSE(err.needsRetry());
}

TEST(SslError_CertificateLoadFailed) {
    SslError err(SslErrorCode::kCertificateLoadFailed);
    EXPECT_FALSE(err.isSuccess());
    EXPECT_FALSE(err.needsRetry());
}

TEST(SslError_PrivateKeyLoadFailed) {
    SslError err(SslErrorCode::kPrivateKeyLoadFailed);
    EXPECT_FALSE(err.isSuccess());
    EXPECT_FALSE(err.needsRetry());
}

// ==================== 主函数 ====================

int main(int /*argc*/, char** /*argv*/) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  galay-ssl Unit Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 测试已在静态初始化时运行

    std::cout << "\n========================================" << std::endl;
    std::cout << "  Test Results" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Total:  " << g_tests_run << std::endl;
    std::cout << "Passed: " << g_tests_passed << std::endl;
    std::cout << "Failed: " << g_tests_failed << std::endl;
    std::cout << "========================================\n" << std::endl;

    return g_tests_failed > 0 ? 1 : 0;
}
