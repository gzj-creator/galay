#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_service.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::rpc;

namespace {

Task<void> noopUnary(RpcContext&)
{
    co_return;
}

Task<void> noopStream(RpcStream&)
{
    co_return;
}

class SurfaceService : public RpcService {
public:
    SurfaceService() : RpcService("SurfaceService") {}

    void addUnary(std::string_view name) { registerUnaryMethod(name, noopUnary); }
    void addClientStreaming(std::string_view name) { registerClientStreamingMethod(name, noopUnary); }
    void addServerStreaming(std::string_view name) { registerServerStreamingMethod(name, noopUnary); }
    void addBidiStreaming(std::string_view name) { registerBidiStreamingMethod(name, noopUnary); }
    void addStreamSession(std::string_view name) { registerStreamMethod(name, noopStream); }
};

bool containsOnce(const std::vector<std::string>& names, const std::string& name)
{
    return std::count(names.begin(), names.end(), name) == 1;
}

void test_same_name_slots_by_call_mode(test::TestResultWriter& writer)
{
    SurfaceService service;
    service.addUnary("shared");
    service.addClientStreaming("shared");
    service.addServerStreaming("shared");
    service.addBidiStreaming("shared");

    const bool passed =
        service.findMethod("shared", RpcCallMode::UNARY) != nullptr &&
        service.findMethod("shared", RpcCallMode::CLIENT_STREAMING) != nullptr &&
        service.findMethod("shared", RpcCallMode::SERVER_STREAMING) != nullptr &&
        service.findMethod("shared", RpcCallMode::BIDI_STREAMING) != nullptr &&
        service.findMethod("shared") == service.findMethod("shared", RpcCallMode::UNARY);

    writer.writeTestCase("same method name registered in four call-mode slots", passed);
}

void test_wrong_mode_not_found(test::TestResultWriter& writer)
{
    SurfaceService service;
    service.addClientStreaming("clientOnly");
    service.addServerStreaming("serverOnly");

    const bool passed =
        service.findMethod("clientOnly", RpcCallMode::SERVER_STREAMING) == nullptr &&
        service.findMethod("serverOnly", RpcCallMode::CLIENT_STREAMING) == nullptr &&
        service.findMethod("clientOnly", RpcCallMode::BIDI_STREAMING) == nullptr &&
        service.findMethod("serverOnly", RpcCallMode::UNARY) == nullptr;

    writer.writeTestCase("wrong call mode method lookup misses", passed);
}

void test_stream_method_names_deduplicated(test::TestResultWriter& writer)
{
    SurfaceService service;
    service.addUnary("dupe");
    service.addClientStreaming("dupe");
    service.addStreamSession("dupe");
    service.addStreamSession("streamOnly");

    const auto names = service.methodNames();

    writer.writeTestCase("registerStreamMethod methodNames deduplicates",
        containsOnce(names, "dupe") &&
        containsOnce(names, "streamOnly") &&
        service.findStreamMethod("dupe") != nullptr &&
        service.findStreamMethod("streamOnly") != nullptr);
}

} // namespace

int main()
{
    test::TestResultWriter writer("t6_service_surface.result");
    std::cout << "Running RPC service surface tests...\n";

    test_same_name_slots_by_call_mode(writer);
    test_wrong_mode_not_found(writer);
    test_stream_method_names_deduplicated(writer);

    writer.writeSummary();
    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";
    return writer.failed() > 0 ? 1 : 0;
}
