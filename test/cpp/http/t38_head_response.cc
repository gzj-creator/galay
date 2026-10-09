#include <galay/cpp/galay-http/kernel/http_session.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template<typename SocketType>
void check_head_response(bool serialized, const std::string& framing)
{
    SocketType socket = [] {
        if constexpr (std::is_same_v<SocketType, galay::async::AsyncTcpSocket>) {
            return SocketType(GHandle::invalid());
        } else {
            return SocketType(nullptr, GHandle::invalid());
        }
    }();
    galay::http::HttpReaderSetting setting;
    setting.set_max_body_size(4);
    galay::http::HttpSessionImpl<SocketType> session(socket, 1024, setting);
    using State = galay::http::detail::HttpSessionState<SocketType>;
    galay::http::HttpRequest request;
    request.header().method() = galay::http::HttpMethod::HEAD;
    request.header().uri() = "/";
    State state = serialized ? State(session, std::string("HEAD / HTTP/1.1\r\n\r\n"))
                             : State(session, std::move(request));
    const std::string first = "HTTP/1.1 200 OK\r\n" + framing;
    require(session.get_ring_buffer().try_write_batch(first.data(), first.size()) == first.size(),
            "seed partial HEAD headers");
    require(!state.parse_from_ring_buffer(), "partial HEAD headers must remain incomplete");
    std::string next_response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    const std::string last = "\r\n\r\n" + next_response;
    require(session.get_ring_buffer().try_write_batch(last.data(), last.size()) == last.size(),
            "seed HEAD terminator and next response");
    require(state.parse_from_ring_buffer(), "HEAD must complete without a body or EOF");
    auto result = state.take_result();
    require(result && result->has_value(), "HEAD must return a response");
    auto& response = **result;
    require(response.is_complete() && response.body_str().empty(), "HEAD response must be complete and empty");
    if (framing.starts_with("Content-Length")) {
        require(response.header().header_pairs().get_value("content-length") == "42",
                "HEAD metadata length must be preserved and not limited as a body");
    } else {
        require(response.header().header_pairs().get_value("transfer-encoding") == "chunked",
                "HEAD transfer metadata must be preserved");
    }
    auto copy = response.clone();
    require(copy.is_complete() && copy.body_str().empty(), "clone must preserve HEAD completion");
    require(session.get_ring_buffer().readable() == next_response.size(),
            "HEAD must not consume bytes belonging to the next response");
    std::vector<iovec> next_iovecs{{next_response.data(), next_response.size()}};
    const auto [head_error, head_consumed] = response.from_io_vec(next_iovecs, 4, galay::http::HttpMethod::HEAD);
    require(head_error == galay::http::kNoError && head_consumed == 0,
            "completed HEAD must not consume another response on repeated parsing");
    galay::http::HttpRequest next_request;
    next_request.header().method() = galay::http::HttpMethod::GET;
    State next_state(session, std::move(next_request));
    require(next_state.parse_from_ring_buffer(), "next response must parse after HEAD");
    const auto next_result = next_state.take_result();
    require(next_result && next_result->has_value() && (**next_result).body_str() == "ok" &&
            session.get_ring_buffer().readable() == 0, "HEAD must preserve the next response body");
    copy.reset();
    std::string get_response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n";
    std::vector<iovec> iovecs{{get_response.data(), get_response.size()}};
    const auto [error, consumed] = copy.from_io_vec(iovecs);
    require(error == galay::http::kNoError && consumed == static_cast<ssize_t>(get_response.size()) &&
            !copy.is_complete(), "reset must restore normal body parsing");
}

} // namespace

int main()
{
    for (const bool serialized : {false, true}) {
        for (const std::string framing : {"Content-Length: 42", "Transfer-Encoding: chunked"}) {
            check_head_response<galay::async::AsyncTcpSocket>(serialized, framing);
#ifdef GALAY_SSL_FEATURE_ENABLED
            check_head_response<galay::ssl::SslSocket>(serialized, framing);
#endif
        }
    }
    return 0;
}
