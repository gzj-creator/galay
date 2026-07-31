#include "bounded_channel_c.h"

#include "../../../cpp/galay-kernel/concurrency/bounded_channel.h"
#include "../coro-c/coro_wait_c.h"

#include <chrono>
#include <limits>
#include <new>
#include <utility>

namespace
{

using CppBoundedChannel = galay::kernel::BoundedChannel<C_BoundedChannelMessage>;

CppBoundedChannel* to_cpp_channel(galay_kernel_bounded_channel_t* channel)
{
    return static_cast<CppBoundedChannel*>(channel->channel);
}

const CppBoundedChannel* to_cpp_channel(const galay_kernel_bounded_channel_t* channel)
{
    return static_cast<const CppBoundedChannel*>(channel->channel);
}

C_IOResult make_result(C_IOResultCode code, size_t bytes = 0, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, bytes, 0, nullptr};
}

bool is_valid_message(const C_BoundedChannelMessage& message)
{
    return message.data != nullptr || message.size == 0;
}

bool is_valid_batch(const C_BoundedChannelMessage* messages, size_t count)
{
    if (count == 0) {
        return true;
    }
    if (messages == nullptr) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!is_valid_message(messages[i])) {
            return false;
        }
    }
    return true;
}

bool is_valid_capacity(size_t capacity)
{
    constexpr size_t max_power_of_two =
        size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    return capacity <= max_power_of_two;
}

std::chrono::steady_clock::time_point make_deadline(int64_t timeout_ms)
{
    if (timeout_ms < 0) {
        return std::chrono::steady_clock::time_point::max();
    }

    const auto now = std::chrono::steady_clock::now();
    const auto max_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    if (timeout_ms >= max_timeout.count()) {
        return std::chrono::steady_clock::time_point::max();
    }
    return now + std::chrono::milliseconds(timeout_ms);
}

bool timeout_expired(std::chrono::steady_clock::time_point deadline, int64_t timeout_ms)
{
    return timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline;
}

C_IOResult suspend_before_retry()
{
    C_IOResult slept = galay_coro_sleep(1);
    if (slept.code == C_IOResultOk ||
        slept.code == C_IOResultCancelled ||
        slept.code == C_IOResultInvalid) {
        return slept;
    }
    return make_result(C_IOResultError, 0, slept.sys_errno);
}

/**
 * @brief 对已校验通道发送一条已校验消息。
 * @param channel 有效的内部 C++ 通道。
 * @param message 有效的非拥有消息结构。
 * @return 成功、关闭或已满状态；不会返回参数错误。
 */
C_BoundedChannelResultCode try_send_message(
    CppBoundedChannel* channel,
    const C_BoundedChannelMessage& message)
{
    C_BoundedChannelMessage copy = message;
    if (channel->trySend(std::move(copy))) {
        return C_BoundedChannelSuccess;
    }
    // trySend() 已在热路径入口检查关闭状态；仅在失败后重读，以区分 Closed 与 Full，
    // 避免每次成功发送都额外执行一次 acquire load。收益由 b27 同口径压测验证。
    return channel->isClosed() ? C_BoundedChannelClosed : C_BoundedChannelFull;
}

/**
 * @brief 从已校验通道非阻塞接收一条消息。
 * @param channel 有效的内部 C++ 通道。
 * @param message 输出消息，未收到消息时清零。
 * @return 成功、关闭或为空状态；不会返回参数错误。
 */
C_BoundedChannelResultCode try_recv_message(
    CppBoundedChannel* channel,
    C_BoundedChannelMessage& message)
{
    message = C_BoundedChannelMessage{};
    auto received = channel->tryRecv();
    if (received.has_value()) {
        message = std::move(*received);
        return C_BoundedChannelSuccess;
    }
    return channel->isClosed() ? C_BoundedChannelClosed : C_BoundedChannelEmpty;
}

} // namespace

const char* galay_kernel_bounded_channel_get_error(C_BoundedChannelResultCode code)
{
    switch (code)
    {
    case C_BoundedChannelSuccess:
        return "success";
    case C_BoundedChannelParameterInvalid:
        return "parameter invalid";
    case C_BoundedChannelMemoryAllocFailed:
        return "memory allocation failed";
    case C_BoundedChannelClosed:
        return "channel closed";
    case C_BoundedChannelFull:
        return "channel full";
    case C_BoundedChannelEmpty:
        return "channel empty";
    }
    return "unknown bounded channel error";
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_create(
    galay_kernel_bounded_channel_t* c_channel,
    size_t capacity)
{
    if (c_channel == nullptr || !is_valid_capacity(capacity)) {
        return C_BoundedChannelParameterInvalid;
    }

    c_channel->channel = nullptr;
    auto* channel = new (std::nothrow) CppBoundedChannel(capacity);
    if (channel == nullptr) {
        return C_BoundedChannelMemoryAllocFailed;
    }
    c_channel->channel = channel;
    return C_BoundedChannelSuccess;
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_destroy(
    galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr) {
        return C_BoundedChannelParameterInvalid;
    }

    if (c_channel->channel != nullptr) {
        to_cpp_channel(c_channel)->close();
        delete to_cpp_channel(c_channel);
        c_channel->channel = nullptr;
    }
    return C_BoundedChannelSuccess;
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_try_send(
    galay_kernel_bounded_channel_t* c_channel,
    const C_BoundedChannelMessage* message)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        message == nullptr || !is_valid_message(*message)) {
        return C_BoundedChannelParameterInvalid;
    }

    return try_send_message(to_cpp_channel(c_channel), *message);
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_try_send_batch(
    galay_kernel_bounded_channel_t* c_channel,
    const C_BoundedChannelMessage* messages,
    size_t count,
    size_t* out_count)
{
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        out_count == nullptr || !is_valid_batch(messages, count)) {
        return C_BoundedChannelParameterInvalid;
    }

    CppBoundedChannel* channel = to_cpp_channel(c_channel);
    for (size_t i = 0; i < count; ++i) {
        C_BoundedChannelResultCode sent = try_send_message(channel, messages[i]);
        if (sent != C_BoundedChannelSuccess) {
            return sent;
        }
        ++(*out_count);
    }
    return C_BoundedChannelSuccess;
}

C_IOResult galay_kernel_bounded_channel_send(
    galay_kernel_bounded_channel_t* c_channel,
    const C_BoundedChannelMessage* message,
    int64_t timeout_ms)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        message == nullptr || !is_valid_message(*message)) {
        return make_result(C_IOResultInvalid);
    }

    CppBoundedChannel* channel = to_cpp_channel(c_channel);
    const auto deadline = make_deadline(timeout_ms);
    for (;;) {
        C_BoundedChannelResultCode sent = try_send_message(channel, *message);
        if (sent == C_BoundedChannelSuccess) {
            return make_result(C_IOResultOk, 1);
        }
        if (sent == C_BoundedChannelClosed) {
            return make_result(C_IOResultCancelled);
        }
        if (sent != C_BoundedChannelFull) {
            return make_result(C_IOResultInvalid);
        }
        if (timeout_ms == 0 || timeout_expired(deadline, timeout_ms)) {
            return make_result(C_IOResultTimeout);
        }

        C_IOResult suspended = suspend_before_retry();
        if (suspended.code != C_IOResultOk) {
            return suspended;
        }
    }
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_try_recv(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* message)
{
    if (message != nullptr) {
        *message = C_BoundedChannelMessage{};
    }
    if (c_channel == nullptr || c_channel->channel == nullptr || message == nullptr) {
        return C_BoundedChannelParameterInvalid;
    }

    return try_recv_message(to_cpp_channel(c_channel), *message);
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_try_recv_batch(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* messages,
    size_t max_count,
    size_t* out_count)
{
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        messages == nullptr || max_count == 0 || out_count == nullptr) {
        return C_BoundedChannelParameterInvalid;
    }

    CppBoundedChannel* channel = to_cpp_channel(c_channel);
    C_BoundedChannelResultCode terminal = C_BoundedChannelEmpty;
    while (*out_count < max_count) {
        C_BoundedChannelResultCode received =
            try_recv_message(channel, messages[*out_count]);
        if (received != C_BoundedChannelSuccess) {
            terminal = received;
            break;
        }
        ++(*out_count);
    }
    return *out_count > 0 ? C_BoundedChannelSuccess : terminal;
}

C_IOResult galay_kernel_bounded_channel_recv(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* message,
    int64_t timeout_ms)
{
    if (message != nullptr) {
        *message = C_BoundedChannelMessage{};
    }
    if (c_channel == nullptr || c_channel->channel == nullptr || message == nullptr) {
        return make_result(C_IOResultInvalid);
    }

    CppBoundedChannel* channel = to_cpp_channel(c_channel);
    const auto deadline = make_deadline(timeout_ms);
    for (;;) {
        C_BoundedChannelResultCode received = try_recv_message(channel, *message);
        if (received == C_BoundedChannelSuccess) {
            return make_result(C_IOResultOk, 1);
        }
        if (received == C_BoundedChannelClosed) {
            return make_result(C_IOResultCancelled);
        }
        if (received != C_BoundedChannelEmpty) {
            return make_result(C_IOResultInvalid);
        }
        if (timeout_ms == 0 || timeout_expired(deadline, timeout_ms)) {
            return make_result(C_IOResultTimeout);
        }

        C_IOResult suspended = suspend_before_retry();
        if (suspended.code != C_IOResultOk) {
            return suspended;
        }
    }
}

C_IOResult galay_kernel_bounded_channel_recv_batch(
    galay_kernel_bounded_channel_t* c_channel,
    C_BoundedChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms)
{
    if (out_count != nullptr) {
        *out_count = 0;
    }
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        messages == nullptr || max_count == 0 || out_count == nullptr) {
        return make_result(C_IOResultInvalid);
    }

    const auto deadline = make_deadline(timeout_ms);
    for (;;) {
        C_BoundedChannelResultCode received =
            galay_kernel_bounded_channel_try_recv_batch(
                c_channel, messages, max_count, out_count);
        if (received == C_BoundedChannelSuccess) {
            return make_result(C_IOResultOk, *out_count);
        }
        if (received == C_BoundedChannelClosed) {
            return make_result(C_IOResultCancelled);
        }
        if (received != C_BoundedChannelEmpty) {
            return make_result(C_IOResultInvalid);
        }
        if (timeout_ms == 0 || timeout_expired(deadline, timeout_ms)) {
            return make_result(C_IOResultTimeout);
        }

        C_IOResult suspended = suspend_before_retry();
        if (suspended.code != C_IOResultOk) {
            return suspended;
        }
    }
}

C_BoundedChannelResultCode galay_kernel_bounded_channel_close(
    galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr) {
        return C_BoundedChannelParameterInvalid;
    }

    to_cpp_channel(c_channel)->close();
    return C_BoundedChannelSuccess;
}

bool galay_kernel_bounded_channel_is_closed(
    const galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr) {
        return true;
    }
    return to_cpp_channel(c_channel)->isClosed();
}

size_t galay_kernel_bounded_channel_capacity(
    const galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr) {
        return 0;
    }
    return to_cpp_channel(c_channel)->capacity();
}

size_t galay_kernel_bounded_channel_size(
    const galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr) {
        return 0;
    }
    return to_cpp_channel(c_channel)->size();
}

bool galay_kernel_bounded_channel_empty(
    const galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr) {
        return true;
    }
    return to_cpp_channel(c_channel)->empty();
}

bool galay_kernel_bounded_channel_full(
    const galay_kernel_bounded_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr) {
        return false;
    }
    return to_cpp_channel(c_channel)->full();
}
