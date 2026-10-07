#include "ssl_await.h"
#include "ssl_socket.h"
#include "../common/ssl_log.h"
#include <algorithm>
#include <limits>
#include <string_view>

namespace galay::ssl
{

namespace {

constexpr size_t kCipherBufSize = 16384;
constexpr size_t kMaxDrainBytes = 64 * 1024;

bool ensure_buffer_size(std::vector<char>& buffer, size_t required)
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

size_t drain_chunk_size(size_t pending)
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

void SslOperationDriver::reset_contexts()
{
    m_recv_context.m_buffer = nullptr;
    m_recv_context.m_length = 0;
    m_send_context.m_buffer = nullptr;
    m_send_context.m_length = 0;
}

void SslOperationDriver::reset_handshake_state()
{
    m_handshake = HandshakeState{};
}

void SslOperationDriver::reset_recv_state()
{
    m_recv = RecvState{};
}

void SslOperationDriver::reset_send_state()
{
    m_send = SendState{};
}

void SslOperationDriver::reset_shutdown_state()
{
    m_shutdown = ShutdownState{};
}

void SslOperationDriver::clear_operation()
{
    m_operation = OperationKind::kNone;
    reset_contexts();
}

void SslOperationDriver::clear_transient_buffers()
{
    m_handshake_buffer.clear();
    m_shutdown_buffer.clear();
    m_recv_cipher_buffer.clear();
    m_send_cipher_buffer.clear();
    reset_contexts();
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

std::expected<void, SslError> SslOperationDriver::take_handshake_result()
{
    auto result = m_handshake.result_set
        ? std::move(m_handshake.result)
        : std::unexpected(SslError(SslErrorCode::kHandshakeFailed));
    reset_handshake_state();
    clear_operation();
    clear_transient_buffers();
    return result;
}

std::expected<Bytes, SslError> SslOperationDriver::take_recv_result()
{
    auto result = m_recv.result_set
        ? std::move(m_recv.result)
        : std::unexpected(SslError(SslErrorCode::kReadFailed));
    reset_recv_state();
    clear_operation();
    clear_transient_buffers();
    return result;
}

std::expected<size_t, SslError> SslOperationDriver::take_send_result()
{
    auto result = m_send.result_set
        ? std::move(m_send.result)
        : std::unexpected(SslError(SslErrorCode::kWriteFailed));
    reset_send_state();
    clear_operation();
    clear_transient_buffers();
    return result;
}

std::expected<void, SslError> SslOperationDriver::take_shutdown_result()
{
    auto result = m_shutdown.result_set
        ? std::move(m_shutdown.result)
        : std::expected<void, SslError>{};
    reset_shutdown_state();
    clear_operation();
    clear_transient_buffers();
    return result;
}

void SslOperationDriver::set_handshake_failure(SslError error)
{
    SSL_LOG_ERROR("[driver] [handshake]", "code={} detail={}", static_cast<uint32_t>(error.code()), error.ssl_error_string());
    m_handshake.result = std::unexpected(std::move(error));
    m_handshake.result_set = true;
    m_handshake.flush_success = false;
    m_handshake.wait_read_after_write = false;
    m_handshake.read_pending = false;
    reset_contexts();
}

void SslOperationDriver::set_recv_failure(SslError error)
{
    m_recv.result = std::unexpected(std::move(error));
    m_recv.result_set = true;
    reset_contexts();
}

void SslOperationDriver::set_send_failure(SslError error)
{
    m_send.result = std::unexpected(std::move(error));
    m_send.result_set = true;
    reset_contexts();
}

void SslOperationDriver::set_shutdown_success()
{
    m_shutdown.result = {};
    m_shutdown.result_set = true;
    m_shutdown.wait_read_after_write = false;
    m_shutdown.read_pending = false;
    reset_contexts();
}

void SslOperationDriver::start_handshake()
{
    clear_operation();
    reset_handshake_state();
    reset_recv_state();
    reset_send_state();
    reset_shutdown_state();
    m_operation = OperationKind::kHandshake;

    if (m_socket == nullptr || !m_socket->is_valid() || !m_socket->init_engine()) {
        SSL_LOG_ERROR("[driver] [init]", "SslEngine initialization failed");
        set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
    }
}

void SslOperationDriver::start_recv(char* buffer, size_t length)
{
    clear_operation();
    reset_handshake_state();
    reset_recv_state();
    reset_send_state();
    reset_shutdown_state();
    m_operation = OperationKind::kRecv;
    m_recv.plain_buffer = buffer;
    m_recv.plain_length = length;

    if (m_socket == nullptr || !m_socket->is_valid() || !m_socket->m_engineInitialized) {
        set_recv_failure(SslError(SslErrorCode::kReadFailed));
        return;
    }
    if (length == 0) {
        m_recv.result = Bytes();
        m_recv.result_set = true;
    }
}

void SslOperationDriver::start_send(const char* buffer, size_t length)
{
    clear_operation();
    reset_handshake_state();
    reset_recv_state();
    reset_send_state();
    reset_shutdown_state();
    m_operation = OperationKind::kSend;
    m_send.plain_buffer = buffer;
    m_send.plain_length = length;

    if (m_socket == nullptr || !m_socket->is_valid() || !m_socket->m_engineInitialized) {
        set_send_failure(SslError(SslErrorCode::kWriteFailed));
        return;
    }
    if (length == 0) {
        m_send.result = size_t{0};
        m_send.result_set = true;
    }
}

void SslOperationDriver::start_shutdown()
{
    clear_operation();
    reset_handshake_state();
    reset_recv_state();
    reset_send_state();
    reset_shutdown_state();
    m_operation = OperationKind::kShutdown;

    if (m_socket == nullptr || !m_socket->is_valid() || !m_socket->m_engineInitialized) {
        set_shutdown_success();
    }
}

bool SslOperationDriver::prepare_read_buffer(std::vector<char>& buffer)
{
    if (!ensure_buffer_size(buffer, kCipherBufSize)) {
        return false;
    }
    m_recv_context.m_buffer = buffer.data();
    m_recv_context.m_length = buffer.size();
    return true;
}

bool SslOperationDriver::prepare_write_from_pending(std::vector<char>& buffer, size_t pending, SslErrorCode error_code)
{
    (void)error_code;
    if (pending == 0) {
        return false;
    }

    const size_t desired = drain_chunk_size(pending);
    if (!ensure_buffer_size(buffer, desired)) {
        return false;
    }

    const size_t to_read = std::min(pending, buffer.size());
    const auto extracted = m_socket->m_engine.extract_encrypted_output(buffer.data(), to_read);
    if (!extracted || *extracted == 0) {
        return false;
    }

    m_send_context.m_buffer = buffer.data();
    m_send_context.m_length = *extracted;
    return true;
}

SslOperationDriver::RecvPollAction SslOperationDriver::drain_recv_plaintext()
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
                m_recv.result = Bytes::from_string(
                    std::string_view(m_recv.plain_buffer, total_read)
                );
                m_recv.result_set = true;
                return RecvPollAction::kCompleted;
            }
            return RecvPollAction::kNeedSend;
        }

        if (ssl_ret == SslIOResult::ZeroReturn) {
            if (total_read > 0) {
                m_recv.result = Bytes::from_string(
                    std::string_view(m_recv.plain_buffer, total_read)
                );
            } else {
                m_recv.result = Bytes();
            }
            m_recv.result_set = true;
            return RecvPollAction::kCompleted;
        }

        if (total_read > 0) {
            m_recv.result = Bytes::from_string(
                std::string_view(m_recv.plain_buffer, total_read)
            );
            m_recv.result_set = true;
        } else {
            set_recv_failure(SslError::from_open_ssl(SslErrorCode::kReadFailed));
        }
        return RecvPollAction::kCompleted;
    }

    if (total_read > 0) {
        m_recv.result = Bytes::from_string(
            std::string_view(m_recv.plain_buffer, total_read)
        );
        m_recv.result_set = true;
        return RecvPollAction::kCompleted;
    }

    return RecvPollAction::kNeedRecv;
}

bool SslOperationDriver::prepare_recv_send_chunk(size_t pending)
{
    if (m_send_context.m_length > 0) {
        return true;
    }

    if (pending == 0) {
        pending = m_socket->m_engine.pending_encrypted_output();
    }
    if (pending == 0) {
        set_recv_failure(SslError(SslErrorCode::kReadFailed));
        return false;
    }

    if (!prepare_write_from_pending(m_recv_cipher_buffer, pending, SslErrorCode::kReadFailed)) {
        set_recv_failure(SslError(SslErrorCode::kReadFailed));
        return false;
    }
    return true;
}

bool SslOperationDriver::fill_send_chunk(size_t pending)
{
    while (true) {
        if (m_send_context.m_length > 0) {
            return true;
        }

        if (pending == 0) {
            pending = m_socket->m_engine.pending_encrypted_output();
        }
        if (pending > 0) {
            if (!prepare_write_from_pending(m_send_cipher_buffer, pending, SslErrorCode::kWriteFailed)) {
                set_send_failure(SslError(SslErrorCode::kWriteFailed));
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
            pending = m_socket->m_engine.pending_encrypted_output();
            if (pending > 0) {
                continue;
            }
            m_send.read_pending = true;
            return false;
        }

        if (ssl_ret == SslIOResult::WantWrite) {
            pending = m_socket->m_engine.pending_encrypted_output();
            if (pending > 0) {
                continue;
            }
        }

        set_send_failure(SslError::from_open_ssl(SslErrorCode::kWriteFailed));
        return false;
    }
}

SslOperationDriver::WaitAction SslOperationDriver::poll()
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        return poll_handshake();
    case OperationKind::kRecv:
        return poll_recv();
    case OperationKind::kSend:
        return poll_send();
    case OperationKind::kShutdown:
        return poll_shutdown();
    case OperationKind::kNone:
        return {};
    }
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::poll_handshake()
{
    if (m_handshake.result_set) {
        return {};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }
    if (m_handshake.read_pending) {
        if (!prepare_read_buffer(m_handshake_buffer)) {
            set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
            return {};
        }
        m_handshake.read_pending = false;
        return {&m_recv_context, WaitKind::kRead};
    }

    const SslIOResult ret = m_socket->m_engine.do_handshake();
    switch (ret) {
    case SslIOResult::Success:
        {
            const size_t pending = m_socket->m_engine.pending_encrypted_output();
            if (pending > 0) {
                if (!prepare_write_from_pending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
                    set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
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
            const size_t pending = m_socket->m_engine.pending_encrypted_output();
            if (!prepare_write_from_pending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
                set_handshake_failure(SslError::from_open_ssl(SslErrorCode::kHandshakeFailed));
                return {};
            }
        }
        m_handshake.flush_success = false;
        m_handshake.wait_read_after_write = false;
        return {&m_send_context, WaitKind::kWrite};
    case SslIOResult::WantRead:
        {
            const size_t pending = m_socket->m_engine.pending_encrypted_output();
            if (pending > 0) {
                if (!prepare_write_from_pending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
                    set_handshake_failure(SslError::from_open_ssl(SslErrorCode::kHandshakeFailed));
                    return {};
                }
                m_handshake.wait_read_after_write = true;
                return {&m_send_context, WaitKind::kWrite};
            }
        }
        if (!prepare_read_buffer(m_handshake_buffer)) {
            set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
            return {};
        }
        return {&m_recv_context, WaitKind::kRead};
    case SslIOResult::ZeroReturn:
        set_handshake_failure(SslError(SslErrorCode::kPeerClosed));
        return {};
    case SslIOResult::Syscall:
    case SslIOResult::Error:
        set_handshake_failure(SslError::from_open_ssl(SslErrorCode::kHandshakeFailed));
        return {};
    }

    set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::poll_recv()
{
    if (m_recv.result_set) {
        return {};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }

    switch (drain_recv_plaintext()) {
    case RecvPollAction::kCompleted:
        return {};
    case RecvPollAction::kNeedSend:
        if (!prepare_recv_send_chunk(0)) {
            return {};
        }
        return {&m_send_context, WaitKind::kWrite};
    case RecvPollAction::kNeedRecv:
        if (!prepare_read_buffer(m_recv_cipher_buffer)) {
            set_recv_failure(SslError(SslErrorCode::kReadFailed));
            return {};
        }
        return {&m_recv_context, WaitKind::kRead};
    }

    set_recv_failure(SslError(SslErrorCode::kReadFailed));
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::poll_send()
{
    if (m_send.result_set) {
        return {};
    }
    if (m_send.read_pending) {
        if (!prepare_read_buffer(m_send_cipher_buffer)) {
            set_send_failure(SslError(SslErrorCode::kWriteFailed));
            return {};
        }
        m_send.read_pending = false;
        return {&m_recv_context, WaitKind::kRead};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }
    if (fill_send_chunk()) {
        return {&m_send_context, WaitKind::kWrite};
    }
    return {};
}

SslOperationDriver::WaitAction SslOperationDriver::poll_shutdown()
{
    if (m_shutdown.result_set) {
        return {};
    }
    if (m_send_context.m_length > 0) {
        return {&m_send_context, WaitKind::kWrite};
    }
    if (m_shutdown.read_pending) {
        if (!prepare_read_buffer(m_shutdown_buffer)) {
            set_shutdown_success();
            return {};
        }
        m_shutdown.read_pending = false;
        return {&m_recv_context, WaitKind::kRead};
    }

    const SslIOResult ret = m_socket->m_engine.shutdown();
    switch (ret) {
    case SslIOResult::Success:
    case SslIOResult::ZeroReturn:
        set_shutdown_success();
        return {};
    case SslIOResult::WantWrite:
        {
            const size_t pending = m_socket->m_engine.pending_encrypted_output();
            if (!prepare_write_from_pending(m_shutdown_buffer, pending, SslErrorCode::kShutdownFailed)) {
                set_shutdown_success();
                return {};
            }
        }
        m_shutdown.wait_read_after_write = false;
        return {&m_send_context, WaitKind::kWrite};
    case SslIOResult::WantRead:
        {
            const size_t pending = m_socket->m_engine.pending_encrypted_output();
            if (pending > 0) {
                if (!prepare_write_from_pending(m_shutdown_buffer, pending, SslErrorCode::kShutdownFailed)) {
                    set_shutdown_success();
                    return {};
                }
                m_shutdown.wait_read_after_write = true;
                return {&m_send_context, WaitKind::kWrite};
            }
        }
        if (!prepare_read_buffer(m_shutdown_buffer)) {
            set_shutdown_success();
            return {};
        }
        return {&m_recv_context, WaitKind::kRead};
    case SslIOResult::Syscall:
    case SslIOResult::Error:
        set_shutdown_success();
        return {};
    }

    set_shutdown_success();
    return {};
}

void SslOperationDriver::on_read(std::expected<size_t, IOError> result)
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        on_handshake_read(std::move(result));
        return;
    case OperationKind::kRecv:
        on_recv_read(std::move(result));
        return;
    case OperationKind::kSend:
        on_send_read(std::move(result));
        return;
    case OperationKind::kShutdown:
        on_shutdown_read(std::move(result));
        return;
    case OperationKind::kNone:
        set_send_failure(SslError(SslErrorCode::kWriteFailed));
        return;
    }
}

void SslOperationDriver::on_write(std::expected<size_t, IOError> result)
{
    switch (m_operation) {
    case OperationKind::kHandshake:
        on_handshake_write(std::move(result));
        return;
    case OperationKind::kRecv:
        on_recv_write(std::move(result));
        return;
    case OperationKind::kSend:
        on_send_write(std::move(result));
        return;
    case OperationKind::kShutdown:
        on_shutdown_write(std::move(result));
        return;
    case OperationKind::kNone:
        return;
    }
}

void SslOperationDriver::on_handshake_read(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
        return;
    }

    const auto fed = m_socket->m_engine.feed_encrypted_input(m_recv_context.m_buffer, result.value());
    if (!fed || *fed == 0) {
        set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
    }
}

void SslOperationDriver::on_handshake_write(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
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

    const size_t pending = m_socket->m_engine.pending_encrypted_output();
    if (pending > 0) {
        if (!prepare_write_from_pending(m_handshake_buffer, pending, SslErrorCode::kHandshakeFailed)) {
            set_handshake_failure(SslError(SslErrorCode::kHandshakeFailed));
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

void SslOperationDriver::on_recv_read(std::expected<size_t, IOError> result)
{
    if (!result) {
        if (IOError::contains(result.error().code(), kDisconnectError)) {
            m_recv.result = Bytes();
            m_recv.result_set = true;
        } else {
            set_recv_failure(SslError(SslErrorCode::kReadFailed));
        }
        return;
    }

    if (result.value() == 0) {
        m_recv.result = Bytes();
        m_recv.result_set = true;
        return;
    }

    const auto fed = m_socket->m_engine.feed_encrypted_input(m_recv_context.m_buffer, result.value());
    if (!fed || *fed == 0) {
        set_recv_failure(SslError(SslErrorCode::kReadFailed));
    }
}

void SslOperationDriver::on_recv_write(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_recv_failure(SslError(SslErrorCode::kReadFailed));
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

    const size_t pending = m_socket->m_engine.pending_encrypted_output();
    if (pending > 0) {
        prepare_recv_send_chunk(pending);
    }
}

void SslOperationDriver::on_send_read(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_send_failure(SslError(SslErrorCode::kWriteFailed));
        return;
    }

    const auto fed = m_socket->m_engine.feed_encrypted_input(m_recv_context.m_buffer, result.value());
    if (!fed || *fed == 0) {
        set_send_failure(SslError(SslErrorCode::kWriteFailed));
    }
}

void SslOperationDriver::on_send_write(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_send_failure(SslError(SslErrorCode::kWriteFailed));
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

    const size_t pending = m_socket->m_engine.pending_encrypted_output();
    if (pending > 0) {
        fill_send_chunk(pending);
    }
}

void SslOperationDriver::on_shutdown_read(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_shutdown_success();
        return;
    }

    const auto fed = m_socket->m_engine.feed_encrypted_input(m_recv_context.m_buffer, result.value());
    if (!fed || *fed == 0) {
        set_shutdown_success();
    }
}

void SslOperationDriver::on_shutdown_write(std::expected<size_t, IOError> result)
{
    if (!result || result.value() == 0) {
        set_shutdown_success();
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

    const size_t pending = m_socket->m_engine.pending_encrypted_output();
    if (pending > 0) {
        if (!prepare_write_from_pending(m_shutdown_buffer, pending, SslErrorCode::kShutdownFailed)) {
            set_shutdown_success();
        }
        return;
    }

    if (m_shutdown.wait_read_after_write) {
        m_shutdown.wait_read_after_write = false;
        m_shutdown.read_pending = true;
    }
}

} // namespace galay::ssl
