#include <cassert>
#include <iostream>
#include <string>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <sstream>

#define private public
#include <galay/cpp/galay-http/kernel/http_writer.h>
#undef private
#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#endif

int main() {
#ifdef GALAY_SSL_FEATURE_ENABLED
    using namespace galay::http;

    galay::ssl::SslSocket socket(nullptr);
    HttpWriterImpl<galay::ssl::SslSocket> writer(HttpWriterSetting(), socket);

    auto response = Http1_1ResponseBuilder()
        .status(HttpStatusCode::OK_200)
        .header("Content-Type", "text/plain")
        .body("ssl-response-body")
        .buildMove();
    const auto expected_response = response.toString();

    const auto ssl_hits_before_response = writer.m_fast_path_counters.ssl_coalesced_layout_hits;
    (void) writer.sendResponse(response);
    if (std::string(writer.bufferData(), writer.getRemainingBytes()) != expected_response) {
        std::cerr << "[T64] ssl response send layout mismatch\n";
        return 1;
    }
    if (writer.m_fast_path_counters.ssl_coalesced_layout_hits != ssl_hits_before_response + 1) {
        std::cerr << "[T64] ssl response send should count one coalesced-layout hit\n";
        return 1;
    }

    galay::http::detail::HttpSslSendMachine<galay::ssl::SslSocket> response_machine(&writer);
    auto first = response_machine.advance();
    assert(first.signal == galay::ssl::SslMachineSignal::kSend);
    assert(first.write_length == expected_response.size());

    response_machine.onSend(std::expected<size_t, galay::ssl::SslError>(first.write_length - 3));
    auto resumed = response_machine.advance();
    assert(resumed.signal == galay::ssl::SslMachineSignal::kSend);
    assert(resumed.write_length == 3);

    response_machine.onSend(std::expected<size_t, galay::ssl::SslError>(0));
    auto failed = response_machine.advance();
    if (failed.signal != galay::ssl::SslMachineSignal::kComplete ||
        !failed.result.has_value() ||
        failed.result->has_value()) {
        std::cerr << "[T64] zero-byte SSL send after partial response progress should fail\n";
        return 1;
    }

    if (writer.getRemainingBytes() != 0 || writer.sentBytes() != 0) {
        std::cerr << "[T64] failed ssl response send should clear pending state\n";
        return 1;
    }

    auto request = Http1_1RequestBuilder::post("/echo")
        .header("Host", "127.0.0.1:8443")
        .header("Content-Type", "text/plain")
        .body("ssl-request-body")
        .buildMove();
    const auto expected_request = request.toString();

    const auto ssl_hits_before_request = writer.m_fast_path_counters.ssl_coalesced_layout_hits;
    (void) writer.sendRequest(request);
    if (std::string(writer.bufferData(), writer.getRemainingBytes()) != expected_request) {
        std::cerr << "[T64] ssl request send layout mismatch\n";
        return 1;
    }
    if (writer.m_fast_path_counters.ssl_coalesced_layout_hits != ssl_hits_before_request + 1) {
        std::cerr << "[T64] ssl request send should count one coalesced-layout hit\n";
        return 1;
    }
#endif

    std::cout << "T64-HttpsWriterSteadyState PASS\n";
    return 0;
}
