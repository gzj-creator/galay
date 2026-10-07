/**
 * @file t10_resume.cc
 * @brief 用途：锁定 SSL send 状态机在等待读事件时，读完成必须回灌密文而不是直接写失败。
 * 关键覆盖点：`SslOperationDriver::poll_send()`、`SslOperationDriver::on_read()` 的 `OperationKind::kSend` 分支。
 * 通过条件：send 挂起后读回对端 TLS record，不会得到 `kWriteFailed`，且密文会被成功喂回引擎。
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-ssl/async/ssl_await.h>
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#undef private

#include <galay/cpp/galay-ssl/ssl/ssl_context.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace galay::ssl;

namespace {

constexpr std::string_view kIncomingPayload = "peer-application-record";
constexpr std::string_view kOutgoingPayload = "local-send-payload";

void expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void replace_socket_fd(SslSocket& socket, int fd, bool is_server)
{
    if (socket.handle().fd >= 0) {
        expect(::close(socket.handle().fd) == 0, "close previous socket failed");
    }
    socket.controller()->m_handle.fd = fd;
    socket.m_isServer = is_server;
    socket.m_engineInitialized = false;
    expect(socket.init_engine(), "initEngine failed");
}

void transfer_pending(SslEngine& from, SslEngine& to)
{
    while (from.pending_encrypted_output() > 0) {
        std::vector<char> buffer(from.pending_encrypted_output());
        const auto produced = from.extract_encrypted_output(buffer.data(), buffer.size());
        expect(produced && *produced > 0, "extractEncryptedOutput failed");
        const auto fed = to.feed_encrypted_input(buffer.data(), *produced);
        expect(fed && *fed == *produced, "feedEncryptedInput failed");
    }
}

void complete_handshake(SslSocket& client, SslSocket& server)
{
    for (int i = 0; i < 64; ++i) {
        if (!client.is_handshake_completed()) {
            const auto ret = client.m_engine.do_handshake();
            expect(ret == SslIOResult::Success ||
                       ret == SslIOResult::WantRead ||
                       ret == SslIOResult::WantWrite,
                   "client handshake failed");
        }
        transfer_pending(client.m_engine, server.m_engine);

        if (!server.is_handshake_completed()) {
            const auto ret = server.m_engine.do_handshake();
            expect(ret == SslIOResult::Success ||
                       ret == SslIOResult::WantRead ||
                       ret == SslIOResult::WantWrite,
                   "server handshake failed");
        }
        transfer_pending(server.m_engine, client.m_engine);

        if (client.is_handshake_completed() && server.is_handshake_completed()) {
            return;
        }
    }

    throw std::runtime_error("handshake did not complete");
}

std::vector<char> produce_peer_record(SslSocket& peer)
{
    size_t bytes_written = 0;
    const auto ret = peer.m_engine.write(kIncomingPayload.data(), kIncomingPayload.size(), bytes_written);
    expect(ret == SslIOResult::Success, "peer engine write failed");
    expect(bytes_written == kIncomingPayload.size(), "peer engine wrote partial plaintext");
    expect(peer.m_engine.pending_encrypted_output() > 0, "peer engine produced no ciphertext");

    std::vector<char> ciphertext(peer.m_engine.pending_encrypted_output());
    const auto produced = peer.m_engine.extract_encrypted_output(ciphertext.data(), ciphertext.size());
    expect(produced && *produced > 0, "peer extractEncryptedOutput failed");
    ciphertext.resize(*produced);
    return ciphertext;
}

} // namespace

int main()
{
    int fds[2];
    expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");

    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    expect(server_ctx.is_valid(), "server context invalid");
    expect(client_ctx.is_valid(), "client context invalid");
    expect(server_ctx.load_certificate("certs/server.crt").has_value(), "load server cert failed");
    expect(server_ctx.load_private_key("certs/server.key").has_value(), "load server key failed");
    expect(client_ctx.load_ca_certificate("certs/ca.crt").has_value(), "load CA failed");
    client_ctx.set_verify_mode(SslVerifyMode::Peer);

    SslSocket client(&client_ctx);
    SslSocket server(&server_ctx);
    expect(client.set_hostname("localhost").has_value(), "set hostname failed");

    replace_socket_fd(client, fds[0], false);
    replace_socket_fd(server, fds[1], true);
    complete_handshake(client, server);

    auto ciphertext = produce_peer_record(client);

    SslOperationDriver driver(&server);
    driver.start_send(kOutgoingPayload.data(), kOutgoingPayload.size());
    driver.m_send.read_pending = true;

    const auto wait = driver.poll();
    expect(wait.kind == SslOperationDriver::WaitKind::kRead, "send did not wait for read");
    expect(wait.context == &driver.recv_context(), "send read wait used unexpected context");
    expect(ciphertext.size() <= driver.recv_context().m_length, "ciphertext larger than recv buffer");

    std::memcpy(driver.recv_context().m_buffer, ciphertext.data(), ciphertext.size());
    driver.on_read(static_cast<size_t>(ciphertext.size()));

    expect(!driver.m_send.result_set, "send read completion should not fail");

    std::array<char, 128> plaintext{};
    size_t bytes_read = 0;
    const auto read_ret = server.m_engine.read(plaintext.data(), plaintext.size(), bytes_read);
    expect(read_ret == SslIOResult::Success, "server engine did not accept fed record");
    expect(std::string_view(plaintext.data(), bytes_read) == kIncomingPayload, "decrypted payload mismatch");

    return 0;
}
