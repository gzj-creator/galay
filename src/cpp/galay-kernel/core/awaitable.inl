#ifndef GALAY_KERNEL_AWAITABLE_INL
#define GALAY_KERNEL_AWAITABLE_INL

#include "awaitable.h"

namespace galay::kernel {

namespace detail {

/**
 * @brief 保存一次 IO 完成结果，并区分需要继续等待的 kNotReady
 * @tparam ValueT IO 结果值类型
 * @param result 后端返回的完成结果
 * @param stored 上下文持有的结果槽位
 * @return 本次操作已完成时返回 true，仍需继续处理时返回 false
 */
template <typename ValueT>
inline bool store_handle_complete_result(
    std::expected<ValueT, IOError>&& result,
    std::expected<ValueT, IOError>& stored) {
    if (!result && IOError::contains(result.error().code(), kNotReady)) {
        return false;
    }
    stored = std::move(result);
    return true;
}

} // namespace detail

// ============ handle_complete inline implementations ============

#ifdef USE_IOURING

inline bool AcceptIOContext::handle_complete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(io::handle_accept(cqe), m_result);
}

inline bool RecvIOContext::handle_complete(struct io_uring_cqe* cqe,
                                          [[maybe_unused]] GHandle handle) {
    if (cqe != nullptr && cqe->res >= 0 && (cqe->flags & IORING_CQE_F_BUFFER) != 0) {
        return true;
    }
    return detail::store_handle_complete_result(io::handle_recv(cqe, m_buffer), m_result);
}

inline bool SendIOContext::handle_complete(struct io_uring_cqe* cqe,
                                          [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(io::handle_send(cqe), m_result);
}

template <IOEventType Event>
inline bool detail::VectorIOContext<Event>::handle_complete(
    struct io_uring_cqe* cqe,
    [[maybe_unused]] GHandle handle) {
    if constexpr (Event == IOEventType::READV) {
        return detail::store_handle_complete_result(io::handle_readv(cqe), m_result);
    } else {
        return detail::store_handle_complete_result(io::handle_writev(cqe), m_result);
    }
}

inline bool ConnectIOContext::handle_complete(struct io_uring_cqe* cqe,
                                             [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(io::handle_connect(cqe), m_result);
}

inline bool RecvFromIOContext::handle_complete(struct io_uring_cqe* cqe,
                                              [[maybe_unused]] GHandle handle) {
    auto [result, from] = io::handle_recv_from(cqe, m_buffer, m_addr);
    if (!detail::store_handle_complete_result(std::move(result), m_result)) {
        return false;
    }
    if(m_from) { *m_from = std::move(from); }
    return true;
}

inline bool SendToIOContext::handle_complete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(io::handle_send_to(cqe), m_result);
}

inline bool FileReadIOContext::handle_complete(struct io_uring_cqe* cqe,
                                              [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(io::handle_file_read(cqe, m_buffer), m_result);
}

inline bool FileWriteIOContext::handle_complete(struct io_uring_cqe* cqe,
                                               [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(io::handle_file_write(cqe), m_result);
}

inline bool FileWatchIOContext::handle_complete(struct io_uring_cqe* cqe,
                                               [[maybe_unused]] GHandle handle) {
    return detail::store_handle_complete_result(
        io::handle_file_watch(cqe, m_buffer, m_ready_events), m_result);
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
    if (!detail::store_handle_complete_result(std::move(result), m_result)) {
        return false;
    }
    *m_host = std::move(host);
    return true;
}

inline bool RecvIOContext::handle_complete(GHandle handle) {
    return detail::store_handle_complete_result(
        io::handle_recv(handle, m_buffer, m_length), m_result);
}

inline bool SendIOContext::handle_complete(GHandle handle) {
    return detail::store_handle_complete_result(
        io::handle_send(handle, m_buffer, m_length), m_result);
}

template <IOEventType Event>
inline bool detail::VectorIOContext<Event>::handle_complete(GHandle handle) {
    auto* iovecs = const_cast<struct iovec*>(m_iovecs.data());
    const int iovec_count = static_cast<int>(m_iovecs.size());
    if constexpr (Event == IOEventType::READV) {
        return detail::store_handle_complete_result(
            io::handle_readv(handle, iovecs, iovec_count), m_result);
    } else {
        return detail::store_handle_complete_result(
            io::handle_writev(handle, iovecs, iovec_count), m_result);
    }
}

inline bool ConnectIOContext::handle_complete(GHandle handle) {
    return detail::store_handle_complete_result(io::handle_connect(handle, m_host), m_result);
}

inline bool RecvFromIOContext::handle_complete(GHandle handle) {
    auto [result, from] = io::handle_recv_from(handle, m_buffer, m_length);
    if (!detail::store_handle_complete_result(std::move(result), m_result)) {
        return false;
    }
    if(m_from) { *m_from = std::move(from); }
    return true;
}

inline bool SendToIOContext::handle_complete(GHandle handle) {
    return detail::store_handle_complete_result(
        io::handle_send_to(handle, m_buffer, m_length, m_to), m_result);
}

inline bool FileReadIOContext::handle_complete(GHandle handle) {
    return detail::store_handle_complete_result(
        io::handle_file_read(handle, m_buffer, m_length, m_offset), m_result);
}

inline bool FileWriteIOContext::handle_complete(GHandle handle) {
    return detail::store_handle_complete_result(
        io::handle_file_write(handle, m_buffer, m_length, m_offset), m_result);
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
