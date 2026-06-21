/**
 * @file t6_service_surface.cc
 * @brief RPC服务表面测试
 */

#include "result_writer.h"
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {

Task<void> noopMethod(RpcContext&) {
    co_return;
}

Task<void> noopStream(RpcStream&) {
    co_return;
}

class SurfaceService : public RpcService {
public:
    SurfaceService()
        : RpcService("SurfaceService") {
        registerUnaryMethod("shared", noopMethod);
        registerClientStreamingMethod("shared", noopMethod);
        registerServerStreamingMethod("shared", noopMethod);
        registerBidiStreamingMethod("shared", noopMethod);
        registerStreamMethod("shared", noopStream);
    }
};

bool containsOnce(const std::vector<std::string>& names, const std::string& name) {
    return std::count(names.begin(), names.end(), name) == 1;
}

} // namespace

void test_find_method_by_call_mode(test::TestResultWriter& writer) {
    SurfaceService service;

    writer.writeTestCase("RpcService findMethod returns mode-specific handlers",
        service.findMethod("shared", RpcCallMode::UNARY) != nullptr &&
        service.findMethod("shared", RpcCallMode::CLIENT_STREAMING) != nullptr &&
        service.findMethod("shared", RpcCallMode::SERVER_STREAMING) != nullptr &&
        service.findMethod("shared", RpcCallMode::BIDI_STREAMING) != nullptr);
}

void test_missing_method_returns_null(test::TestResultWriter& writer) {
    SurfaceService service;

    writer.writeTestCase("RpcService missing method returns null",
        service.findMethod("missing", RpcCallMode::UNARY) == nullptr &&
        service.findMethod("missing", RpcCallMode::CLIENT_STREAMING) == nullptr);
}

void test_find_stream_method(test::TestResultWriter& writer) {
    SurfaceService service;

    writer.writeTestCase("RpcService findStreamMethod returns stream handler",
        service.findStreamMethod("shared") != nullptr &&
        service.findStreamMethod("missing") == nullptr);
}

void test_method_names_deduplicates_surface(test::TestResultWriter& writer) {
    SurfaceService service;
    const auto names = service.methodNames();

    writer.writeTestCase("RpcService methodNames deduplicates method names",
        containsOnce(names, "shared"));
}

int main() {
    test::TestResultWriter writer("t6_service_surface.result");

    std::cout << "Running RPC Service Surface Tests...\n";

    test_find_method_by_call_mode(writer);
    test_missing_method_returns_null(writer);
    test_find_stream_method(writer);
    test_method_names_deduplicates_surface(writer);

    writer.writeSummary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
