#include <expected>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <galay-utils/cache/bytes.hpp>
#include "galay-kernel/core/runtime.h"
#include "galay-ssl/common/error.h"

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
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
            return m_socket->consumeSend(m_buffer, m_length);
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
            return m_socket->consumeRecv(m_buffer, m_length);
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

    size_t sendCallCount() const { return m_send_call_count; }
    size_t recvCallCount() const { return m_recv_call_count; }

private:
    friend class SendAwaitable;
    friend class RecvAwaitable;

    std::expected<size_t, galay::ssl::SslError> consumeSend(const char* buffer, size_t length) {
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

    std::expected<Bytes, galay::ssl::SslError> consumeRecv(char*, size_t) {
        ++m_recv_call_count;
        return std::unexpected(galay::ssl::SslError(galay::ssl::SslErrorCode::kPeerClosed));
    }

    std::vector<std::expected<size_t, galay::ssl::SslError>> m_send_results;
    std::string m_sent_payload;
    size_t m_send_call_count = 0;
    size_t m_recv_call_count = 0;
};

Task<void> runSslOwnerLoop(Http2StreamManagerImpl<MockSslSocket>& manager) {
    co_await manager.sslServiceLoop(nullptr);
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
    manager.prepareForStart(false);

    auto stream = conn.createStream(1);

    auto waiter1 = std::make_shared<Http2OutgoingFrame::Waiter>();
    auto waiter2 = std::make_shared<Http2OutgoingFrame::Waiter>();
    manager.enqueueSendBytes("frame-one", waiter1);
    manager.enqueueSendBytes("frame-two", waiter2);

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto run_result = runtime.blockOn(static_cast<galay::kernel::Task<void>>(runSslOwnerLoop(manager)));
    runtime.stop();
    if (!run_result) {
        std::cerr << "[T54] runtime.blockOn failed: " << run_result.error().message() << "\n";
        return 1;
    }

    if (!waiter1->isReady() || !waiter2->isReady()) {
        std::cerr << "[T54] ssl owner loop should notify all queued waiters even when batched send fails\n";
        return 1;
    }

    if (conn.socket().sendCallCount() != 1) {
        std::cerr << "[T54] expected one coalesced SSL send attempt, got "
                  << conn.socket().sendCallCount() << "\n";
        return 1;
    }

    if (!stream->isFrameQueueClosed()) {
        std::cerr << "[T54] sslServiceLoop send-fail exit should still close stream queues after batched waiter flush\n";
        return 1;
    }

    if (conn.isClosing()) {
        std::cerr << "[T54] mock send failure should not require transport close just to preserve queue cleanup\n";
        return 1;
    }

    std::cout << "T54-H2TlsSslOwnerLoop PASS\n";
    return 0;
#endif
}
