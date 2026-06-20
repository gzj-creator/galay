#include "flow_control.h"

#include <algorithm>

namespace galay::http2
{

namespace
{

bool wouldExceedMaxWindow(int64_t current, uint32_t increment)
{
    return current > kH2MaxFlowControlWindow - static_cast<int64_t>(increment);
}

} // namespace

std::expected<void, H2FlowControlError> H2FlowController::ensureStream(uint32_t stream_id)
{
    m_window.stream_windows.try_emplace(stream_id, m_window.initial_stream_window);
    return {};
}

std::expected<void, H2FlowControlError> H2FlowController::applyConnectionWindowUpdate(uint32_t increment)
{
    if (wouldExceedMaxWindow(m_window.conn_window, increment)) {
        return std::unexpected(H2FlowControlError::WindowOverflow);
    }
    m_window.conn_window += static_cast<int64_t>(increment);
    return {};
}

std::expected<void, H2FlowControlError> H2FlowController::applyStreamWindowUpdate(uint32_t stream_id,
                                                                                 uint32_t increment)
{
    auto it = m_window.stream_windows.find(stream_id);
    if (it == m_window.stream_windows.end()) {
        return std::unexpected(H2FlowControlError::UnknownStream);
    }
    if (wouldExceedMaxWindow(it->second, increment)) {
        return std::unexpected(H2FlowControlError::WindowOverflow);
    }
    it->second += static_cast<int64_t>(increment);
    return {};
}

std::expected<void, H2FlowControlError> H2FlowController::applyInitialStreamWindowSize(uint32_t new_size)
{
    if (new_size > static_cast<uint32_t>(kH2MaxFlowControlWindow)) {
        return std::unexpected(H2FlowControlError::WindowOverflow);
    }

    const int64_t delta = static_cast<int64_t>(new_size) - m_window.initial_stream_window;
    for (const auto& [stream_id, window] : m_window.stream_windows) {
        (void)stream_id;
        if (window + delta > kH2MaxFlowControlWindow) {
            return std::unexpected(H2FlowControlError::WindowOverflow);
        }
    }

    m_window.initial_stream_window = static_cast<int64_t>(new_size);
    for (auto& [stream_id, window] : m_window.stream_windows) {
        (void)stream_id;
        window += delta;
    }
    return {};
}

size_t H2FlowController::availableToSend(uint32_t stream_id,
                                         size_t requested,
                                         uint32_t max_frame_size) const
{
    auto it = m_window.stream_windows.find(stream_id);
    if (it == m_window.stream_windows.end() ||
        m_window.conn_window <= 0 ||
        it->second <= 0 ||
        requested == 0 ||
        max_frame_size == 0) {
        return 0;
    }

    return std::min<size_t>({
        requested,
        static_cast<size_t>(max_frame_size),
        static_cast<size_t>(m_window.conn_window),
        static_cast<size_t>(it->second)
    });
}

std::expected<void, H2FlowControlError> H2FlowController::consumeSendWindow(uint32_t stream_id,
                                                                           size_t bytes)
{
    auto it = m_window.stream_windows.find(stream_id);
    if (it == m_window.stream_windows.end()) {
        return std::unexpected(H2FlowControlError::UnknownStream);
    }
    if (bytes > static_cast<size_t>(std::max<int64_t>(0, m_window.conn_window)) ||
        bytes > static_cast<size_t>(std::max<int64_t>(0, it->second))) {
        return std::unexpected(H2FlowControlError::InsufficientWindow);
    }

    const auto consumed = static_cast<int64_t>(bytes);
    m_window.conn_window -= consumed;
    it->second -= consumed;
    return {};
}

} // namespace galay::http2
