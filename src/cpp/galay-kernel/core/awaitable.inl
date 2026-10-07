#ifndef GALAY_KERNEL_AWAITABLE_INL
#define GALAY_KERNEL_AWAITABLE_INL

#include "awaitable.h"

namespace galay::kernel {

// ============ handle_complete inline implementations ============

#ifdef USE_IOURING

inline bool AcceptIOContext::handle_complete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    auto result = io::handle_accept(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool RecvIOContext::handle_complete(struct io_uring_cqe* cqe,
                                          [[maybe_unused]] GHandle handle) {
    if (cqe != nullptr && cqe->res >= 0 && (cqe->flags & IORING_CQE_F_BUFFER) != 0) {
        return true;
    }
    auto result = io::handle_recv(cqe, m_buffer);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool SendIOContext::handle_complete(struct io_uring_cqe* cqe,
                                          [[maybe_unused]] GHandle handle) {
    auto result = io::handle_send(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ReadvIOContext::handle_complete(struct io_uring_cqe* cqe,
                                           [[maybe_unused]] GHandle handle) {
    auto result = io::handle_readv(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool WritevIOContext::handle_complete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    auto result = io::handle_writev(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ConnectIOContext::handle_complete(struct io_uring_cqe* cqe,
                                             [[maybe_unused]] GHandle handle) {
    auto result = io::handle_connect(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool RecvFromIOContext::handle_complete(struct io_uring_cqe* cqe,
                                              [[maybe_unused]] GHandle handle) {
    auto [result, from] = io::handle_recv_from(cqe, m_buffer, m_addr);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    if(m_from) { *m_from = std::move(from); }
    return true;
}

inline bool SendToIOContext::handle_complete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    auto result = io::handle_send_to(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileReadIOContext::handle_complete(struct io_uring_cqe* cqe,
                                              [[maybe_unused]] GHandle handle) {
    auto result = io::handle_file_read(cqe, m_buffer);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWriteIOContext::handle_complete(struct io_uring_cqe* cqe,
                                               [[maybe_unused]] GHandle handle) {
    auto result = io::handle_file_write(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWatchIOContext::handle_complete(struct io_uring_cqe* cqe,
                                               [[maybe_unused]] GHandle handle) {
    auto result = io::handle_file_watch(cqe, m_buffer, m_ready_events);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool SendFileIOContext::handle_complete(struct io_uring_cqe* cqe, GHandle handle) {
    while (m_count > 0) {
        auto result = io::handle_send_file(cqe, handle, m_file_fd, m_offset, m_count);
        if (!result) {
            if (IOError::contains(result.error().code(), kNotReady)) {
                return false;
            }
            auto& stored = (m_result = std::unexpected(result.error()));
            return !stored.has_value();
        }

        const size_t sent = result.value();
        if (sent == 0) {
            auto& stored = (m_result = m_transferred);
            return stored.has_value();
        }
        m_offset += static_cast<off_t>(sent);
        m_count -= sent;
        m_transferred += sent;
    }

    auto& stored = (m_result = m_transferred);
    return stored.has_value();
}

#else // kqueue / epoll

inline bool AcceptIOContext::handle_complete(GHandle handle) {
    auto [result, host] = io::handle_accept(handle);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    *m_host = std::move(host);
    return true;
}

inline bool RecvIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_recv(handle, m_buffer, m_length);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool SendIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_send(handle, m_buffer, m_length);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ReadvIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_readv(handle,
                                  const_cast<struct iovec*>(m_iovecs.data()),
                                  static_cast<int>(m_iovecs.size()));
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool WritevIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_writev(handle,
                                   const_cast<struct iovec*>(m_iovecs.data()),
                                   static_cast<int>(m_iovecs.size()));
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ConnectIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_connect(handle, m_host);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool RecvFromIOContext::handle_complete(GHandle handle) {
    auto [result, from] = io::handle_recv_from(handle, m_buffer, m_length);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    if(m_from) { *m_from = std::move(from); }
    return true;
}

inline bool SendToIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_send_to(handle, m_buffer, m_length, m_to);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileReadIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_file_read(handle, m_buffer, m_length, m_offset);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWriteIOContext::handle_complete(GHandle handle) {
    auto result = io::handle_file_write(handle, m_buffer, m_length, m_offset);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWatchIOContext::handle_complete([[maybe_unused]] GHandle handle) {
    return true;
}

inline bool SendFileIOContext::handle_complete(GHandle handle) {
    while (m_count > 0) {
        auto result = io::handle_send_file(handle, m_file_fd, m_offset, m_count);
        if (!result) {
            if (IOError::contains(result.error().code(), kNotReady)) {
                return false;
            }
            auto& stored = (m_result = std::unexpected(result.error()));
            return !stored.has_value();
        }

        const size_t sent = result.value();
        if (sent == 0) {
            auto& stored = (m_result = m_transferred);
            return stored.has_value();
        }
        m_offset += static_cast<off_t>(sent);
        m_count -= sent;
        m_transferred += sent;
    }

    auto& stored = (m_result = m_transferred);
    return stored.has_value();
}

#endif // USE_IOURING

}

#endif
