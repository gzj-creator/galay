#include <expected>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <galay/cpp/galay-utils/buffer/bytes.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-ssl/common/error.h>

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

using namespace galay::http2;
using namespace galay::kernel;
using ::galay::utils::Bytes;

namespace {

class MockSslSocket {
public:
    MockSslSocket() = default;

    explicit MockSslSocket(std::vector<std::expected<size_t, galay::ssl::SslError>> send_results)
        : m_send_results(std::move(send_results)) {}

    MockSslSocket(const MockSslSocket&) = delete;
    MockSslSocket& operator=(const MockSslSocket&) = delete;
    MockSslSocket(MockSslSocket&&) noexcept = default;
    MockSslSocket& operator=(MockSslSocket&&) noexcept = default;

    class SendAwaitable {
    public:
        SendAwaitable(MockSslSocket* socket, const char* buffer, size_t length)
            : m_socket(socket)
            , m_buffer(buffer)
            , m_length(length) {}

        bool await_ready() const noexcept { return true; }

        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise>) noexcept {
            return false;
        }

        std::expected<size_t, galay::ssl::SslError> await_resume() {
            return m_socket->consume_send(m_buffer, m_length);
        }

    private:
        MockSslSocket* m_socket;
        const char* m_buffer;
        size_t m_length;
    };

    class RecvAwaitable {
    public:
        RecvAwaitable(MockSslSocket* socket, char* buffer, size_t length)
            : m_socket(socket)
            , m_buffer(buffer)
            , m_length(length) {}

        RecvAwaitable&& timeout(std::chrono::milliseconds) && {
            return std::move(*this);
        }

        bool await_ready() const noexcept { return true; }

        template<typename Promise>
        bool await_suspend(std::coroutine_handle<Promise>) noexcept {
            return false;
        }

        std::expected<Bytes, galay::ssl::SslError> await_resume() {
            return m_socket->consume_recv(m_buffer, m_length);
        }

    private:
        MockSslSocket* m_socket;
        char* m_buffer;
        size_t m_length;
    };

    SendAwaitable send(const char* buffer, size_t length) {
        return SendAwaitable(this, buffer, length);
    }

    RecvAwaitable recv(char* buffer, size_t length) {
        return RecvAwaitable(this, buffer, length);
    }

    GHandle handle() const noexcept { return GHandle::invalid(); }

    size_t send_call_count() const { return m_send_call_count; }
    size_t recv_call_count() const { return m_recv_call_count; }

private:
    friend class SendAwaitable;
    friend class RecvAwaitable;

    std::expected<size_t, galay::ssl::SslError> consume_send(const char* buffer, size_t length) {
        ++m_send_call_count;
        if (!m_send_results.empty()) {
            auto result = std::move(m_send_results.front());
            m_send_results.erase(m_send_results.begin());
            if (result) {
                const size_t written = std::min(result.value(), length);
                m_sent_payload.append(buffer, written);
            }
            return result;
        }

        m_sent_payload.append(buffer, length);
        return length;
    }

    std::expected<Bytes, galay::ssl::SslError> consume_recv(char*, size_t) {
        ++m_recv_call_count;
        return std::unexpected(galay::ssl::SslError(galay::ssl::SslErrorCode::kPeerClosed));
    }

    std::vector<std::expected<size_t, galay::ssl::SslError>> m_send_results;
    std::string m_sent_payload;
    size_t m_send_call_count = 0;
    size_t m_recv_call_count = 0;
};

Task<void> run_ssl_owner_loop(Http2StreamManagerImpl<MockSslSocket>& manager) {
    co_await manager.ssl_service_loop(nullptr);
    co_return;
}

} // namespace
#endif

int main() {
#ifndef GALAY_SSL_FEATURE_ENABLED
    std::cout << "T54-H2TlsSslOwnerLoop SKIP (SSL disabled)\n";
    return 0;
#else
    auto send_error = std::unexpected(galay::ssl::SslError(galay::ssl::SslErrorCode::kWriteFailed));
    Http2ConnImpl<MockSslSocket> conn(MockSslSocket({
        send_error,
    }));
    Http2StreamManagerImpl<MockSslSocket> manager(conn);
    manager.prepare_for_start(false);

    auto stream = conn.create_stream(1);

    auto waiter1 = std::make_shared<Http2OutgoingFrame::Waiter>();
    auto waiter2 = std::make_shared<Http2OutgoingFrame::Waiter>();
    manager.enqueue_send_bytes("frame-one", waiter1);
    manager.enqueue_send_bytes("frame-two", waiter2);

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();
    auto run_result = runtime.block_on_io(static_cast<galay::kernel::Task<void>>(run_ssl_owner_loop(manager)));
    runtime.stop();
    if (!run_result) {
        std::cerr << "[T54] runtime.blockOn failed: " << run_result.error().message() << "\n";
        return 1;
    }

    if (!waiter1->is_ready() || !waiter2->is_ready()) {
        std::cerr << "[T54] ssl owner loop should notify all queued waiters even when batched send fails\n";
        return 1;
    }

    if (conn.socket().send_call_count() != 1) {
        std::cerr << "[T54] expected one coalesced SSL send attempt, got "
                  << conn.socket().send_call_count() << "\n";
        return 1;
    }

    if (!stream->is_frame_queue_closed()) {
        std::cerr << "[T54] sslServiceLoop send-fail exit should still close stream queues after batched waiter flush\n";
        return 1;
    }

    if (conn.is_closing()) {
        std::cerr << "[T54] mock send failure should not require transport close just to preserve queue cleanup\n";
        return 1;
    }

    std::cout << "T54-H2TlsSslOwnerLoop PASS\n";
    return 0;
#endif
}
