#include "result_writer.h"

#include <galay/cpp/galay-rpc/protoc/rpc_codec.h>
#include <galay/cpp/galay-rpc/protoc/rpc_error.h>

#include <string>

using namespace galay::rpc;

namespace {

void expect(test::TestResultWriter& writer, const std::string& name, bool passed) {
    writer.write_test_case(name, passed);
}

bool round_trip_error(RpcErrorCode code) {
    RpcResponse response(77, code);
    response.payload("error-payload", 13);
    auto serialized = response.serialize();
    auto decoded = RpcCodec::decode_response(serialized.data(), serialized.size());
    return decoded.has_value() &&
           decoded->request_id() == 77 &&
           decoded->error_code() == code &&
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
           std::string(rpc_error_code_to_string(RpcErrorCode::OK)) == "OK" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::UNKNOWN_ERROR)) == "Unknown error" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::SERVICE_NOT_FOUND)) == "Service not found" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::METHOD_NOT_FOUND)) == "Method not found" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::INVALID_REQUEST)) == "Invalid request" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::INVALID_RESPONSE)) == "Invalid response" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::REQUEST_TIMEOUT)) == "Request timeout" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::CONNECTION_CLOSED)) == "Connection closed" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::SERIALIZATION_ERROR)) == "Serialization error" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::DESERIALIZATION_ERROR)) == "Deserialization error" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::INTERNAL_ERROR)) == "Internal error");

    expect(writer, "new error strings are stable",
           std::string(rpc_error_code_to_string(RpcErrorCode::CANCELLED)) == "Cancelled" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::DEADLINE_EXCEEDED)) == "Deadline exceeded" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::RESOURCE_EXHAUSTED)) == "Resource exhausted" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::RATE_LIMITED)) == "Rate limited" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::CIRCUIT_OPEN)) == "Circuit open" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::UNAUTHENTICATED)) == "Unauthenticated" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::PERMISSION_DENIED)) == "Permission denied" &&
           std::string(rpc_error_code_to_string(RpcErrorCode::UNAVAILABLE)) == "Unavailable");

    expect(writer, "new error codes round trip through response codec",
           round_trip_error(RpcErrorCode::CANCELLED) &&
           round_trip_error(RpcErrorCode::DEADLINE_EXCEEDED) &&
           round_trip_error(RpcErrorCode::RESOURCE_EXHAUSTED) &&
           round_trip_error(RpcErrorCode::RATE_LIMITED) &&
           round_trip_error(RpcErrorCode::CIRCUIT_OPEN) &&
           round_trip_error(RpcErrorCode::UNAUTHENTICATED) &&
           round_trip_error(RpcErrorCode::PERMISSION_DENIED) &&
           round_trip_error(RpcErrorCode::UNAVAILABLE));

    RpcError timeout = RpcError(RpcErrorCode::DEADLINE_EXCEEDED);
    expect(writer, "deadline error default message uses new error string",
           timeout.message() == "Deadline exceeded");

    writer.write_summary();
    return writer.failed() == 0 ? 0 : 1;
}
