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

Task<void> noop_method(RpcContext&) {
    co_return;
}

Task<void> noop_stream(RpcStream&) {
    co_return;
}

class SurfaceService : public RpcService {
public:
    SurfaceService()
        : RpcService("SurfaceService") {
        register_unary_method("shared", noop_method);
        register_client_streaming_method("shared", noop_method);
        register_server_streaming_method("shared", noop_method);
        register_bidi_streaming_method("shared", noop_method);
        register_stream_method("shared", noop_stream);
    }
};

bool contains_once(const std::vector<std::string>& names, const std::string& name) {
    return std::count(names.begin(), names.end(), name) == 1;
}

} // namespace

void test_find_method_by_call_mode(test::TestResultWriter& writer) {
    SurfaceService service;

    writer.write_test_case("RpcService findMethod returns mode-specific handlers",
        service.find_method("shared", RpcCallMode::UNARY) != nullptr &&
        service.find_method("shared", RpcCallMode::CLIENT_STREAMING) != nullptr &&
        service.find_method("shared", RpcCallMode::SERVER_STREAMING) != nullptr &&
        service.find_method("shared", RpcCallMode::BIDI_STREAMING) != nullptr);
}

void test_missing_method_returns_null(test::TestResultWriter& writer) {
    SurfaceService service;

    writer.write_test_case("RpcService missing method returns null",
        service.find_method("missing", RpcCallMode::UNARY) == nullptr &&
        service.find_method("missing", RpcCallMode::CLIENT_STREAMING) == nullptr);
}

void test_find_stream_method(test::TestResultWriter& writer) {
    SurfaceService service;

    writer.write_test_case("RpcService findStreamMethod returns stream handler",
        service.find_stream_method("shared") != nullptr &&
        service.find_stream_method("missing") == nullptr);
}

void test_method_names_deduplicates_surface(test::TestResultWriter& writer) {
    SurfaceService service;
    const auto names = service.method_names();

    writer.write_test_case("RpcService methodNames deduplicates method names",
        contains_once(names, "shared"));
}

int main() {
    test::TestResultWriter writer("t6_service_surface.result");

    std::cout << "Running RPC Service Surface Tests...\n";

    test_find_method_by_call_mode(writer);
    test_missing_method_returns_null(writer);
    test_find_stream_method(writer);
    test_method_names_deduplicates_surface(writer);

    writer.write_summary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
