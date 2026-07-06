#include "ssl_await.h"
#include "ssl_socket.h"
#include <galay/cpp/galay-ssl/common/ssl_log.h>
#include <algorithm>
#include <limits>
#include <string_view>

namespace galay::ssl
{

namespace {

constexpr size_t kCipherBufSize = 16384;
constexpr size_t kMaxDrainBytes = 64 * 1024;

bool ensureBufferSize(std::vector<char>& buffer, size_t required)
{
    if (required == 0 || required <= buffer.size()) {
        return true;
    }

    size_t new_size = buffer.size();
    if (new_size < kCipherBufSize) {
        new_size = kCipherBufSize;
    }

    while (new_size < required) {
        if (new_size > std::numeric_limits<size_t>::max() / 2) {
            new_size = required;
            break;
        }
        new_size *= 2;
    }

    if (new_size < required) {
        return false;
    }

    buffer.resize(new_size);
    return true;
}

size_t drainChunkSize(size_t pending)
{
    if (pending == 0) {
        return kCipherBufSize;
    }
    return std::max(kCipherBufSize, std::min(pending, kMaxDrainBytes));
}

} // namespace

SslOperationDriver::SslOperationDriver(SslSocket* socket)
    : m_socket(socket)
    , m_recv_context(nullptr, 0)
    , m_send_context(nullptr, 0)
{}

void SslOperationDriver::resetContexts()
{
    m_recv_context.m_buffer = nullptr;
    m_recv_context.m_length = 0;
    m_send_context.m_buffer = nullptr;
    m_send_context.m_length = 0;
}

void SslOperationDriver::resetHandshakeState()
{
    m_handshake = HandshakeState{};
}

void SslOperationDriver::resetRecvState()
{
    m_recv = RecvState{};
}

void SslOperationDriver::resetSendState()
{
    m_send = SendState{};
}

void SslOperationDriver::resetShutdownState()
{
    m_shutdown = ShutdownState{};
}

void SslOperationDriver::clearOperation()
{
    m_operation = OperationKind::kNone;
    resetContexts();
}

void SslOperationDriver::clearTransientBuffers()
{
    m_handshake_buffer.clear();
    m_shutdown_buffer.clear();
    m_recv_cipher_buffer.clear();
    m_send_cipher_buffer.clear();
    resetContexts();
}

bool SslOperationDriver::completed() const
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        return m_handshake.result_set;
    case OperationKind::kRecv:
        return m_recv.result_set;
    case OperationKind::kSend:
        return m_send.result_set;
    case OperationKind::kShutdown:
        return m_shutdown.result_set;
    case OperationKind::kNone:
        return false;
    }
    return false;
}

std::expected<void, SslError> SslOperationDriver::takeHandshakeResult()
{
    auto result = m_handshake.result_set
        ? std::move(m_handshake.result)
        : std::unexpected(SslError(SslErrorCode::kHandshakeFailed));
    resetHandshakeState();
    clearOperation();
    clearTransientBuffers();
    return result;
}

std::expected<Bytes, SslError> SslOperationDriver::takeRecvResult()
{
    auto result = m_recv.result_set
        ? std::move(m_recv.result)
        : std::unexpected(SslError(SslErrorCode::kReadFailed));
    resetRecvState();
    clearOperation();
    clearTransientBuffers();
    return result;
}

std::expected<size_t, SslError> SslOperationDriver::takeSendResult()
{
    auto result = m_send.result_set
        ? std::move(m_send.result)
        : std::unexpected(SslError(SslErrorCode::kWriteFailed));
    resetSendState();
    clearOperation();
    clearTransientBuffers();
    return result;
}

std::expected<void, SslError> SslOperationDriver::takeShutdownResult()
{
    auto result = m_shutdown.result_set
        ? std::move(m_shutdown.result)
        : std::expected<void, SslError>{};
    resetShutdownState();
    clearOperation();
    clearTransientBuffers();
    return result;
}

void SslOperationDriver::setHandshakeFailure(SslError error)
{
    SSL_LOG_ERROR("[driver] [handshake]", "code={} detail={}", static_cast<uint32_t>(error.code()), error.sslErrorString());
    m_handshake.result = std::unexpected(std::move(error));
    m_handshake.result_set = true;
    m_handshake.flush_success = false;
    m_handshake.wait_read_after_write = false;
    m_handshake.read_pending = false;
    resetContexts();
}

void SslOperationDriver::setRecvFailure(SslError error)
{
    m_recv.result = std::unexpected(std::move(error));
    m_recv.result_set = true;
    resetContexts();
}

void SslOperationDriver::setSendFailure(SslError error)
{
    m_send.result = std::unexpected(std::move(error));
    m_send.result_set = true;
    resetContexts();
}

void SslOperationDriver::setShutdownSuccess()
{
    m_shutdown.result = {};
    m_shutdown.result_set = true;
    m_shutdown.wait_read_after_write = false;
    m_shutdown.read_pending = false;
    resetContexts();
}

void SslOperationDriver::startHandshake()
{
    clearOperation();
    resetHandshakeState();
    resetRecvState();
    resetSendState();
    resetShutdownState();
    m_operation = OperationKind::kHandshake;

    if (m_socket == nullptr || !m_socket->isValid() || !m_socket->initEngine()) {
        SSL_LOG_ERROR("[driver] [init]", "SslEngine initialization failed");
        setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
    }
}

void SslOperationDriver::startRecv(char* buffer, size_t length)
{
    clearOperation();
    resetHandshakeState();
    resetRecvState();
    resetSendState();
    resetShutdownState();
    m_operation = OperationKind::kRecv;
    m_recv.plain_buffer = buffer;
    m_recv.plain_length = length;

    if (m_socket == nullptr || !m_socket->isValid() || !m_socket->m_engineInitialized) {
        setRecvFailure(SslError(SslErrorCode::kReadFailed));
        return;
    }
    if (length == 0) {
        m_recv.result = Bytes();
        m_recv.result_set = true;
    }
}

void SslOperationDriver::startSend(const char* buffer, size_t length)
{
    clearOperation();
    resetHandshakeState();
    resetRecvState();
    resetSendState();
    resetShutdownState();
    m_operation = OperationKind::kSend;
    m_send.plain_buffer = buffer;
    m_send.plain_length = length;

    if (m_socket == nullptr || !m_socket->isValid() || !m_socket->m_engineInitialized) {
        setSendFailure(SslError(SslErrorCode::kWriteFailed));
        return;
    }
    if (length == 0) {
        m_send.result = size_t{0};
        m_send.result_set = true;
    }
}

void SslOperationDriver::startShutdown()
{
    clearOperation();
    resetHandshakeState();
    resetRecvState();
    resetSendState();
    resetShutdownState();
    m_operation = OperationKind::kShutdown;

    if (m_socket == nullptr || !m_socket->isValid() || !m_socket->m_engineInitialized) {
        setShutdownSuccess();
    }
}

bool SslOperationDriver::prepareReadBuffer(std::vector<char>& buffer)
{
    if (!ensureBufferSize(buffer, kCipherBufSize)) {
        return false;
    }
    m_recv_context.m_buffer = buffer.data();
    m_recv_context.m_length = buffer.size();
    return true;
}

bool SslOperationDriver::prepareWriteFromPending(std::vector<char>& buffer, size_t pending, SslErrorCode error_code)
{
    (void)error_code;
    if (pending == 0) {
        return false;
    }

    const size_t desired = drainChunkSize(pending);
    if (!ensureBufferSize(buffer, desired)) {
        return false;
    }

    const size_t to_read = std::min(pending, buffer.size());
    const int n = m_socket->m_engine.extractEncryptedOutput(buffer.data(), to_read);
    if (n <= 0) {
        return false;
    }

    m_send_context.m_buffer = buffer.data();
    m_send_context.m_length = static_cast<size_t>(n);
    return true;
}

SslOperationDriver::RecvPollAction SslOperationDriver::drainRecvPlaintext()
{
    size_t total_read = 0;
    while (total_read < m_recv.plain_length) {
        size_t bytes_read = 0;
        const SslIOResult ssl_ret = m_socket->m_engine.read(
            m_recv.plain_buffer + total_read,
            m_recv.plain_length - total_read,
            bytes_read
        );

        if (ssl_ret == SslIOResult::Success && bytes_read > 0) {
            total_read += bytes_read;
            continue;
        }

        if (ssl_ret == SslIOResult::WantRead) {
            break;
        }

        if (ssl_ret == SslIOResult::WantWrite) {
            if (total_read > 0) {
                m_recv.result = Bytes::fromString(
                    std::string_view(m_recv.plain_buffer, total_read)
                );
                m_recv.result_set = true;
                return RecvPollAction::kCompleted;
            }
            return RecvPollAction::kNeedSend;
        }

        if (ssl_ret == SslIOResult::ZeroReturn) {
            if (total_read > 0) {
                m_recv.result = Bytes::fromString(
                    std::string_view(m_recv.plain_buffer, total_read)
                );
            } else {
                m_recv.result = Bytes();
            }
            m_recv.result_set = true;
            return RecvPollAction::kCompleted;
        }

        if (total_read > 0) {
            m_recv.result = Bytes::fromString(
                std::string_view(m_recv.plain_buffer, total_read)
            );
            m_recv.result_set = true;
        } else {
            setRecvFailure(SslError::fromOpenSSL(SslErrorCode::kReadFailed));
        }
        return RecvPollAction::kCompleted;
    }

    if (total_read > 0) {
        m_recv.result = Bytes::fromString(
            std::string_view(m_recv.plain_buffer, total_read)
        );
        m_recv.result_set = true;
        return RecvPollAction::kCompleted;
    }

    return RecvPollAction::kNeedRecv;
}

bool SslOperationDriver::prepareRecvSendChunk(size_t pending)
{
    if (m_send_context.m_length > 0) {
        return true;
    }

    if (pending == 0) {
        pending = m_socket->m_engine.pendingEncryptedOutput();
    }
    if (pending == 0) {
        setRecvFailure(SslError(SslErrorCode::kReadFailed));
        return false;
    }

    if (!prepareWriteFromPending(m_recv_cipher_buffer, pending, SslErrorCode::kReadFailed)) {
        setRecvFailure(SslError(SslErrorCode::kReadFailed));
        return false;
    }
    return true;
}

bool SslOperationDriver::fillSendChunk(size_t pending)
{
    while (true) {
        if (m_send_context.m_length > 0) {
            return true;
        }

        if (pending == 0) {
            pending = m_socket->m_engine.pendingEncryptedOutput();
        }
        if (pending > 0) {
            if (!prepareWriteFromPending(m_send_cipher_buffer, pending, SslErrorCode::kWriteFailed)) {
                setSendFailure(SslError(SslErrorCode::kWriteFailed));
                return false;
            }
            return true;
        }

        if (m_send.plain_offset >= m_send.plain_length) {
            m_send.result = m_send.plain_length;
            m_send.result_set = true;
            return false;
        }

        size_t bytes_written = 0;
        const SslIOResult ssl_ret = m_socket->m_engine.write(
            m_send.plain_buffer + m_send.plain_offset,
            m_send.plain_length - m_send.plain_offset,
            bytes_written
        );

        if (ssl_ret == SslIOResult::Success && bytes_written > 0) {
            m_send.plain_offset += bytes_written;
            pending = 0;
            continue;
        }

        if (ssl_ret == SslIOResult::WantRead) {
            pending = m_socket->m_engine.pendingEncryptedOutput();
            if (pending > 0) {
                continue;
            }
            m_send.read_pending = true;
            return false;
        }

        if (ssl_ret == SslIOResult::WantWrite) {
            pending = m_socket->m_engine.pendingEncryptedOutput();
            if (pending > 0) {
                continue;
            }
        }

        setSendFailure(SslError::fromOpenSSL(SslErrorCode::kWriteFailed));
        return false;
    }
}

SslOperationDriver::WaitAction SslOperationDriver::poll()
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        return pollHandshake();
    case OperationKind::kRecv:
        return pollRecv();
    case OperationKind::kSend:
        return pollSend();
    case OperationKind::kShutdown:
        return pollShutdown();
    case OperationKind::kNone:
        return {};
    }
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::pollHandshake()
{
    if (m_handshake.result_set) {
        return {};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }
    if (m_handshake.read_pending) {
        if (!prepareReadBuffer(m_handshake_buffer)) {
            setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
            return {};
        }
        m_handshake.read_pending = false;
        return {&m_recv_context, WaitKind::kRead};
    }

    const SslIOResult ret = m_socket->m_engine.doHandshake();
    switch (ret) {
    case SslIOResult::Success:
        {
            const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
            if (pending > 0) {
                if (!prepareWriteFromPending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
                    setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
                    return {};
                }
                m_handshake.flush_success = true;
                return {&m_send_context, WaitKind::kWrite};
            }
        }
        m_handshake.result = {};
        m_handshake.result_set = true;
        return {};
    case SslIOResult::WantWrite:
        {
            const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
            if (!prepareWriteFromPending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
                setHandshakeFailure(SslError::fromOpenSSL(SslErrorCode::kHandshakeFailed));
                return {};
            }
        }
        m_handshake.flush_success = false;
        m_handshake.wait_read_after_write = false;
        return {&m_send_context, WaitKind::kWrite};
    case SslIOResult::WantRead:
        {
            const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
            if (pending > 0) {
                if (!prepareWriteFromPending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
                    setHandshakeFailure(SslError::fromOpenSSL(SslErrorCode::kHandshakeFailed));
                    return {};
                }
                m_handshake.wait_read_after_write = true;
                return {&m_send_context, WaitKind::kWrite};
            }
        }
        if (!prepareReadBuffer(m_handshake_buffer)) {
            setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
            return {};
        }
        return {&m_recv_context, WaitKind::kRead};
    case SslIOResult::ZeroReturn:
        setHandshakeFailure(SslError(SslErrorCode::kPeerClosed));
        return {};
    case SslIOResult::Syscall:
    case SslIOResult::Error:
        setHandshakeFailure(SslError::fromOpenSSL(SslErrorCode::kHandshakeFailed));
        return {};
    }

    setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::pollRecv()
{
    if (m_recv.result_set) {
        return {};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }

    switch (drainRecvPlaintext()) {
    case RecvPollAction::kCompleted:
        return {};
    case RecvPollAction::kNeedSend:
        if (!prepareRecvSendChunk(0)) {
            return {};
        }
        return {&m_send_context, WaitKind::kWrite};
    case RecvPollAction::kNeedRecv:
        if (!prepareReadBuffer(m_recv_cipher_buffer)) {
            setRecvFailure(SslError(SslErrorCode::kReadFailed));
            return {};
        }
        return {&m_recv_context, WaitKind::kRead};
    }

    setRecvFailure(SslError(SslErrorCode::kReadFailed));
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::pollSend()
{
    if (m_send.result_set) {
        return {};
    }
    if (m_send.read_pending) {
        if (!prepareReadBuffer(m_send_cipher_buffer)) {
            setSendFailure(SslError(SslErrorCode::kWriteFailed));
            return {};
        }
        m_send.read_pending = false;
        return {&m_recv_context, WaitKind::kRead};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }
    if (fillSendChunk()) {
        return {&m_send_context, WaitKind::kWrite};
    }
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::pollShutdown()
{
    if (m_shutdown.result_set) {
        return {};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }
    if (m_shutdown.read_pending) {
        if (!prepareReadBuffer(m_shutdown_buffer)) {
            setShutdownSuccess();
            return {};
        }
        m_shutdown.read_pending = false;
        return {&m_recv_context, WaitKind::kRead};
    }

    const SslIOResult ret = m_socket->m_engine.shutdown();
    switch (ret) {
    case SslIOResult::Success:
    case SslIOResult::ZeroReturn:
        setShutdownSuccess();
        return {};
    case SslIOResult::WantWrite:
        {
            const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
            if (!prepareWriteFromPending(m_shutdown_buffer, pending, SslErrorCode::kShutdownFailed)) {
                setShutdownSuccess();
                return {};
            }
        }
        m_shutdown.wait_read_after_write = false;
        return {&m_send_context, WaitKind::kWrite};
    case SslIOResult::WantRead:
        {
            const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
            if (pending > 0) {
                if (!prepareWriteFromPending(m_shutdown_buffer, pending, SslErrorCode::kShutdownFailed)) {
                    setShutdownSuccess();
                    return {};
                }
                m_shutdown.wait_read_after_write = true;
                return {&m_send_context, WaitKind::kWrite};
            }
        }
        if (!prepareReadBuffer(m_shutdown_buffer)) {
            setShutdownSuccess();
            return {};
        }
        return {&m_recv_context, WaitKind::kRead};
    case SslIOResult::Syscall:
    case SslIOResult::Error:
        setShutdownSuccess();
        return {};
    }

    setShutdownSuccess();
    return {};
}

void SslOperationDriver::onRead(std::expected<size_t, IOError> result)
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        onHandshakeRead(std::move(result));
        return;
    case OperationKind::kRecv:
        onRecvRead(std::move(result));
        return;
    case OperationKind::kSend:
        onSendRead(std::move(result));
        return;
    case OperationKind::kShutdown:
        onShutdownRead(std::move(result));
        return;
    case OperationKind::kNone:
        setSendFailure(SslError(SslErrorCode::kWriteFailed));
        return;
    }
}

void SslOperationDriver::onWrite(std::expected<size_t, IOError> result)
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        onHandshakeWrite(std::move(result));
        return;
    case OperationKind::kRecv:
        onRecvWrite(std::move(result));
        return;
    case OperationKind::kSend:
        onSendWrite(std::move(result));
        return;
    case OperationKind::kShutdown:
        onShutdownWrite(std::move(result));
        return;
    case OperationKind::kNone:
        return;
    }
}

void SslOperationDriver::onHandshakeRead(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
        return;
    }

    if (m_socket->m_engine.feedEncryptedInput(m_recv_context.m_buffer, result.value()) <= 0) {
        setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
    }
}

void SslOperationDriver::onHandshakeWrite(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
        return;
    }

    const size_t sent = result.value();
    if (sent < m_send_context.m_length) {
        m_send_context.m_buffer += sent;
        m_send_context.m_length -= sent;
        return;
    }

    m_send_context.m_buffer += m_send_context.m_length;
    m_send_context.m_length = 0;

    const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
    if (pending > 0) {
        if (!prepareWriteFromPending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
            setHandshakeFailure(SslError(SslErrorCode::kHandshakeFailed));
        }
        return;
    }

    if (m_handshake.flush_success) {
        m_handshake.result = {};
        m_handshake.result_set = true;
        m_handshake.flush_success = false;
        return;
    }

    if (m_handshake.wait_read_after_write) {
        m_handshake.wait_read_after_write = false;
        m_handshake.read_pending = true;
    }
}

void SslOperationDriver::onRecvRead(std::expected<size_t, IOError> result)
{
    if (!result) {
        if (IOError::contains(result.error().code(), kDisconnectError)) {
            m_recv.result = Bytes();
            m_recv.result_set = true;
        } else {
            setRecvFailure(SslError(SslErrorCode::kReadFailed));
        }
        return;
    }

    if (result.value() == 0) {
        m_recv.result = Bytes();
        m_recv.result_set = true;
        return;
    }

    if (m_socket->m_engine.feedEncryptedInput(m_recv_context.m_buffer, result.value()) <= 0) {
        setRecvFailure(SslError(SslErrorCode::kReadFailed));
    }
}

void SslOperationDriver::onRecvWrite(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setRecvFailure(SslError(SslErrorCode::kReadFailed));
        return;
    }

    const size_t sent = result.value();
    if (sent < m_send_context.m_length) {
        m_send_context.m_buffer += sent;
        m_send_context.m_length -= sent;
        return;
    }

    m_send_context.m_buffer += m_send_context.m_length;
    m_send_context.m_length = 0;

    const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
    if (pending > 0) {
        prepareRecvSendChunk(pending);
    }
}

void SslOperationDriver::onSendRead(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setSendFailure(SslError(SslErrorCode::kWriteFailed));
        return;
    }

    if (m_socket->m_engine.feedEncryptedInput(m_recv_context.m_buffer, result.value()) <= 0) {
        setSendFailure(SslError(SslErrorCode::kWriteFailed));
    }
}

void SslOperationDriver::onSendWrite(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setSendFailure(SslError(SslErrorCode::kWriteFailed));
        return;
    }

    const size_t sent = result.value();
    if (sent < m_send_context.m_length) {
        m_send_context.m_buffer += sent;
        m_send_context.m_length -= sent;
        return;
    }

    m_send_context.m_buffer += m_send_context.m_length;
    m_send_context.m_length = 0;

    const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
    if (pending > 0) {
        fillSendChunk(pending);
    }
}

void SslOperationDriver::onShutdownRead(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setShutdownSuccess();
        return;
    }

    if (m_socket->m_engine.feedEncryptedInput(m_recv_context.m_buffer, result.value()) <= 0) {
        setShutdownSuccess();
    }
}

void SslOperationDriver::onShutdownWrite(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        setShutdownSuccess();
        return;
    }

    const size_t sent = result.value();
    if (sent < m_send_context.m_length) {
        m_send_context.m_buffer += sent;
        m_send_context.m_length -= sent;
        return;
    }

    m_send_context.m_buffer += m_send_context.m_length;
    m_send_context.m_length = 0;

    const size_t pending = m_socket->m_engine.pendingEncryptedOutput();
    if (pending > 0) {
        if (!prepareWriteFromPending(m_shutdown_buffer, pending, SslErrorCode::kShutdownFailed)) {
            setShutdownSuccess();
        }
        return;
    }

    if (m_shutdown.wait_read_after_write) {
        m_shutdown.wait_read_after_write = false;
        m_shutdown.read_pending = true;
    }
}

} // namespace galay::ssl
