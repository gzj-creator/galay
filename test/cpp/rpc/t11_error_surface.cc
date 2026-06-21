#include "result_writer.h"

#include <galay/cpp/galay-rpc/protoc/rpc_codec.h>
#include <galay/cpp/galay-rpc/protoc/rpc_error.h>

#include <string>

using namespace galay::rpc;

namespace {

void expect(test::TestResultWriter& writer, const std::string& name, bool passed) {
    writer.writeTestCase(name, passed);
}

bool roundTripError(RpcErrorCode code) {
    RpcResponse response(77, code);
    response.payload("error-payload", 13);
    auto serialized = response.serialize();
    auto decoded = RpcCodec::decodeResponse(serialized.data(), serialized.size());
    return decoded.has_value() &&
           decoded->requestId() == 77 &&
           decoded->errorCode() == code &&
           std::string(decoded->payload().data(), decoded->payload().size()) == "error-payload";
}

}  // namespace

int main() {
    test::TestResultWriter writer("rpc_t11_error_surface_results.txt");

    expect(writer, "existing numeric error codes stay stable",
           static_cast<uint16_t>(RpcErrorCode::OK) == 0 &&
           static_cast<uint16_t>(RpcErrorCode::UNKNOWN_ERROR) == 1 &&
           static_cast<uint16_t>(RpcErrorCode::SERVICE_NOT_FOUND) == 2 &&
           static_cast<uint16_t>(RpcErrorCode::METHOD_NOT_FOUND) == 3 &&
           static_cast<uint16_t>(RpcErrorCode::INVALID_REQUEST) == 4 &&
           static_cast<uint16_t>(RpcErrorCode::INVALID_RESPONSE) == 5 &&
           static_cast<uint16_t>(RpcErrorCode::REQUEST_TIMEOUT) == 6 &&
           static_cast<uint16_t>(RpcErrorCode::CONNECTION_CLOSED) == 7 &&
           static_cast<uint16_t>(RpcErrorCode::SERIALIZATION_ERROR) == 8 &&
           static_cast<uint16_t>(RpcErrorCode::DESERIALIZATION_ERROR) == 9 &&
           static_cast<uint16_t>(RpcErrorCode::INTERNAL_ERROR) == 10);

    expect(writer, "existing error strings stay stable",
           std::string(rpcErrorCodeToString(RpcErrorCode::OK)) == "OK" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::UNKNOWN_ERROR)) == "Unknown error" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::SERVICE_NOT_FOUND)) == "Service not found" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::METHOD_NOT_FOUND)) == "Method not found" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::INVALID_REQUEST)) == "Invalid request" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::INVALID_RESPONSE)) == "Invalid response" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::REQUEST_TIMEOUT)) == "Request timeout" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::CONNECTION_CLOSED)) == "Connection closed" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::SERIALIZATION_ERROR)) == "Serialization error" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::DESERIALIZATION_ERROR)) == "Deserialization error" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::INTERNAL_ERROR)) == "Internal error");

    expect(writer, "new error strings are stable",
           std::string(rpcErrorCodeToString(RpcErrorCode::CANCELLED)) == "Cancelled" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::DEADLINE_EXCEEDED)) == "Deadline exceeded" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::RESOURCE_EXHAUSTED)) == "Resource exhausted" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::RATE_LIMITED)) == "Rate limited" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::CIRCUIT_OPEN)) == "Circuit open" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::UNAUTHENTICATED)) == "Unauthenticated" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::PERMISSION_DENIED)) == "Permission denied" &&
           std::string(rpcErrorCodeToString(RpcErrorCode::UNAVAILABLE)) == "Unavailable");

    expect(writer, "new error codes round trip through response codec",
           roundTripError(RpcErrorCode::CANCELLED) &&
           roundTripError(RpcErrorCode::DEADLINE_EXCEEDED) &&
           roundTripError(RpcErrorCode::RESOURCE_EXHAUSTED) &&
           roundTripError(RpcErrorCode::RATE_LIMITED) &&
           roundTripError(RpcErrorCode::CIRCUIT_OPEN) &&
           roundTripError(RpcErrorCode::UNAUTHENTICATED) &&
           roundTripError(RpcErrorCode::PERMISSION_DENIED) &&
           roundTripError(RpcErrorCode::UNAVAILABLE));

    RpcError timeout = RpcError(RpcErrorCode::DEADLINE_EXCEEDED);
    expect(writer, "deadline error default message uses new error string",
           timeout.message() == "Deadline exceeded");

    writer.writeSummary();
    return writer.failed() == 0 ? 0 : 1;
}
