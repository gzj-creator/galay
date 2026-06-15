/**
 * @file t10_resume.cc
 * @brief 用途：锁定 SSL send 状态机在等待读事件时，读完成必须回灌密文而不是直接写失败。
 * 关键覆盖点：`SslOperationDriver::pollSend()`、`SslOperationDriver::onRead()` 的 `OperationKind::kSend` 分支。
 * 通过条件：send 挂起后读回对端 TLS record，不会得到 `kWriteFailed`，且密文会被成功喂回引擎。
 */

#define private public
#include "galay-ssl/async/ssl_await.h"
#include "galay-ssl/async/ssl_socket.h"
#undef private

#include "galay-ssl/ssl/ssl_context.h"

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

void replaceSocketFd(SslSocket& socket, int fd, bool is_server)
{
    if (socket.m_controller.m_handle.fd >= 0) {
        ::close(socket.m_controller.m_handle.fd);
    }
    socket.m_controller.m_handle.fd = fd;
    socket.m_isServer = is_server;
    socket.m_engineInitialized = false;
    expect(socket.initEngine(), "initEngine failed");
}

void transferPending(SslEngine& from, SslEngine& to)
{
    while (from.pendingEncryptedOutput() > 0) {
        std::vector<char> buffer(from.pendingEncryptedOutput());
        const int produced = from.extractEncryptedOutput(buffer.data(), buffer.size());
        expect(produced > 0, "extractEncryptedOutput failed");
        expect(to.feedEncryptedInput(buffer.data(), static_cast<size_t>(produced)) == produced,
               "feedEncryptedInput failed");
    }
}

void completeHandshake(SslSocket& client, SslSocket& server)
{
    for (int i = 0; i < 64; ++i) {
        if (!client.isHandshakeCompleted()) {
            const auto ret = client.m_engine.doHandshake();
            expect(ret == SslIOResult::Success ||
                       ret == SslIOResult::WantRead ||
                       ret == SslIOResult::WantWrite,
                   "client handshake failed");
        }
        transferPending(client.m_engine, server.m_engine);

        if (!server.isHandshakeCompleted()) {
            const auto ret = server.m_engine.doHandshake();
            expect(ret == SslIOResult::Success ||
                       ret == SslIOResult::WantRead ||
                       ret == SslIOResult::WantWrite,
                   "server handshake failed");
        }
        transferPending(server.m_engine, client.m_engine);

        if (client.isHandshakeCompleted() && server.isHandshakeCompleted()) {
            return;
        }
    }

    throw std::runtime_error("handshake did not complete");
}

std::vector<char> producePeerRecord(SslSocket& peer)
{
    size_t bytes_written = 0;
    const auto ret = peer.m_engine.write(kIncomingPayload.data(), kIncomingPayload.size(), bytes_written);
    expect(ret == SslIOResult::Success, "peer engine write failed");
    expect(bytes_written == kIncomingPayload.size(), "peer engine wrote partial plaintext");
    expect(peer.m_engine.pendingEncryptedOutput() > 0, "peer engine produced no ciphertext");

    std::vector<char> ciphertext(peer.m_engine.pendingEncryptedOutput());
    const int produced = peer.m_engine.extractEncryptedOutput(ciphertext.data(), ciphertext.size());
    expect(produced > 0, "peer extractEncryptedOutput failed");
    ciphertext.resize(static_cast<size_t>(produced));
    return ciphertext;
}

} // namespace

int main()
{
    int fds[2];
    expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");

    SslContext server_ctx(SslMethod::TLS_Server);
    SslContext client_ctx(SslMethod::TLS_Client);
    expect(server_ctx.isValid(), "server context invalid");
    expect(client_ctx.isValid(), "client context invalid");
    expect(server_ctx.loadCertificate("certs/server.crt").has_value(), "load server cert failed");
    expect(server_ctx.loadPrivateKey("certs/server.key").has_value(), "load server key failed");
    expect(client_ctx.loadCACertificate("certs/ca.crt").has_value(), "load CA failed");
    client_ctx.setVerifyMode(SslVerifyMode::Peer);

    SslSocket client(&client_ctx);
    SslSocket server(&server_ctx);
    expect(client.setHostname("localhost").has_value(), "set hostname failed");

    replaceSocketFd(client, fds[0], false);
    replaceSocketFd(server, fds[1], true);
    completeHandshake(client, server);

    auto ciphertext = producePeerRecord(client);

    SslOperationDriver driver(&server);
    driver.startSend(kOutgoingPayload.data(), kOutgoingPayload.size());
    driver.m_send.read_pending = true;

    const auto wait = driver.poll();
    expect(wait.kind == SslOperationDriver::WaitKind::kRead, "send did not wait for read");
    expect(wait.context == &driver.recvContext(), "send read wait used unexpected context");
    expect(ciphertext.size() <= driver.recvContext().m_length, "ciphertext larger than recv buffer");

    std::memcpy(driver.recvContext().m_buffer, ciphertext.data(), ciphertext.size());
    driver.onRead(static_cast<size_t>(ciphertext.size()));

    expect(!driver.m_send.result_set, "send read completion should not fail");

    std::array<char, 128> plaintext{};
    size_t bytes_read = 0;
    const auto read_ret = server.m_engine.read(plaintext.data(), plaintext.size(), bytes_read);
    expect(read_ret == SslIOResult::Success, "server engine did not accept fed record");
    expect(std::string_view(plaintext.data(), bytes_read) == kIncomingPayload, "decrypted payload mismatch");

    return 0;
}
