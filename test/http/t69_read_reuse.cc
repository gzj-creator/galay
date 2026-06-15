#include <iostream>
#include <vector>

#include <sstream>

#define private public
#include "galay-http/kernel/http_reader.h"
#undef private

#include "galay-kernel/async/tcp_socket.h"

int main() {
    using namespace galay::http;
    using namespace galay::async;
    using namespace galay::kernel;
    using ::galay::utils::RingBuffer;

    RingBuffer ring_buffer(4096);
    HttpReaderSetting setting;
    TcpSocket socket(IPType::IPV4);
    HttpReader reader(ring_buffer, setting, socket);

    HttpRequest request1;
    HttpRequest request2;

    (void) reader.getRequest(request1);
    if (!reader.m_request_read_state) {
        std::cerr << "[T69] first getRequest should create cached request state\n";
        return 1;
    }

    auto* first_state = reader.m_request_read_state.get();
    if (first_state->m_request != &request1) {
        std::cerr << "[T69] cached state should bind first request object\n";
        return 1;
    }

    first_state->m_total_received = 123;
    first_state->m_parse_iovecs.push_back({nullptr, 7});
    first_state->m_http_error = HttpError(kRecvError, "stale");

    (void) reader.getRequest(request2);
    if (!reader.m_request_read_state) {
        std::cerr << "[T69] second getRequest should keep cached request state\n";
        return 1;
    }
    if (reader.m_request_read_state.get() != first_state) {
        std::cerr << "[T69] getRequest should reuse the same request state instance\n";
        return 1;
    }
    if (reader.m_request_read_state->m_request != &request2) {
        std::cerr << "[T69] reused state should rebind the request pointer\n";
        return 1;
    }
    if (reader.m_request_read_state->m_total_received != 0) {
        std::cerr << "[T69] reused state should clear accumulated receive bytes\n";
        return 1;
    }
    if (!reader.m_request_read_state->m_parse_iovecs.empty()) {
        std::cerr << "[T69] reused state should clear temporary parse iovecs\n";
        return 1;
    }
    if (reader.m_request_read_state->m_http_error.has_value()) {
        std::cerr << "[T69] reused state should clear previous parse errors\n";
        return 1;
    }

    std::cout << "T69-HttpReaderRequestStateReuse PASS\n";
    return 0;
}
