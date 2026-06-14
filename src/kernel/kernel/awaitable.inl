#ifndef GALAY_KERNEL_AWAITABLE_INL
#define GALAY_KERNEL_AWAITABLE_INL

#include "awaitable.h"

namespace galay::kernel {

// ============ handleComplete inline implementations ============

#ifdef USE_IOURING

inline bool AcceptIOContext::handleComplete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    auto result = io::handleAccept(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool RecvIOContext::handleComplete(struct io_uring_cqe* cqe,
                                          [[maybe_unused]] GHandle handle) {
    auto result = io::handleRecv(cqe, m_buffer);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool SendIOContext::handleComplete(struct io_uring_cqe* cqe,
                                          [[maybe_unused]] GHandle handle) {
    auto result = io::handleSend(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ReadvIOContext::handleComplete(struct io_uring_cqe* cqe,
                                           [[maybe_unused]] GHandle handle) {
    auto result = io::handleReadv(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool WritevIOContext::handleComplete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    auto result = io::handleWritev(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ConnectIOContext::handleComplete(struct io_uring_cqe* cqe,
                                             [[maybe_unused]] GHandle handle) {
    auto result = io::handleConnect(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool RecvFromIOContext::handleComplete(struct io_uring_cqe* cqe,
                                              [[maybe_unused]] GHandle handle) {
    auto [result, from] = io::handleRecvFrom(cqe, m_buffer, m_addr);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    if(m_from) { *m_from = std::move(from); }
    return true;
}

inline bool SendToIOContext::handleComplete(struct io_uring_cqe* cqe,
                                            [[maybe_unused]] GHandle handle) {
    auto result = io::handleSendTo(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileReadIOContext::handleComplete(struct io_uring_cqe* cqe,
                                              [[maybe_unused]] GHandle handle) {
    auto result = io::handleFileRead(cqe, m_buffer);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWriteIOContext::handleComplete(struct io_uring_cqe* cqe,
                                               [[maybe_unused]] GHandle handle) {
    auto result = io::handleFileWrite(cqe);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWatchIOContext::handleComplete(struct io_uring_cqe* cqe,
                                               [[maybe_unused]] GHandle handle) {
    auto result = io::handleFileWatch(cqe, m_buffer);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool SendFileIOContext::handleComplete(struct io_uring_cqe* cqe, GHandle handle) {
    auto result = io::handleSendFile(cqe, handle, m_file_fd, m_offset, m_count);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

#else // kqueue / epoll

inline bool AcceptIOContext::handleComplete(GHandle handle) {
    auto [result, host] = io::handleAccept(handle);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    *m_host = std::move(host);
    return true;
}

inline bool RecvIOContext::handleComplete(GHandle handle) {
    auto result = io::handleRecv(handle, m_buffer, m_length);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool SendIOContext::handleComplete(GHandle handle) {
    auto result = io::handleSend(handle, m_buffer, m_length);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ReadvIOContext::handleComplete(GHandle handle) {
    auto result = io::handleReadv(handle,
                                  const_cast<struct iovec*>(m_iovecs.data()),
                                  static_cast<int>(m_iovecs.size()));
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool WritevIOContext::handleComplete(GHandle handle) {
    auto result = io::handleWritev(handle,
                                   const_cast<struct iovec*>(m_iovecs.data()),
                                   static_cast<int>(m_iovecs.size()));
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool ConnectIOContext::handleComplete(GHandle handle) {
    auto result = io::handleConnect(handle, m_host);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool RecvFromIOContext::handleComplete(GHandle handle) {
    auto [result, from] = io::handleRecvFrom(handle, m_buffer, m_length);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    if(m_from) { *m_from = std::move(from); }
    return true;
}

inline bool SendToIOContext::handleComplete(GHandle handle) {
    auto result = io::handleSendTo(handle, m_buffer, m_length, m_to);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileReadIOContext::handleComplete(GHandle handle) {
    auto result = io::handleFileRead(handle, m_buffer, m_length, m_offset);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWriteIOContext::handleComplete(GHandle handle) {
    auto result = io::handleFileWrite(handle, m_buffer, m_length, m_offset);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

inline bool FileWatchIOContext::handleComplete([[maybe_unused]] GHandle handle) {
    return true;
}

inline bool SendFileIOContext::handleComplete(GHandle handle) {
    auto result = io::handleSendFile(handle, m_file_fd, m_offset, m_count);
    if(!result && IOError::contains(result.error().code(), kNotReady)) return false;
    m_result = std::move(result);
    return true;
}

#endif // USE_IOURING

}

#endif
