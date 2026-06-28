#include "mpsc_channel_c.h"

#include "../../../cpp/galay-kernel/concurrency/mpsc_channel.h"
#include "../coro-c/coro_task_c.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace
{

using CppMpscChannel = galay::kernel::MpscChannel<C_MpscChannelMessage>;

CppMpscChannel* to_cpp_channel(galay_kernel_mpsc_channel_t* channel)
{
    return static_cast<CppMpscChannel*>(channel->channel);
}

const CppMpscChannel* to_cpp_channel(const galay_kernel_mpsc_channel_t* channel)
{
    return static_cast<const CppMpscChannel*>(channel->channel);
}

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

bool is_valid_message(const C_MpscChannelMessage& message)
{
    return message.data != nullptr || message.size == 0;
}

bool is_valid_batch(const C_MpscChannelMessage* messages, size_t count)
{
    if (count == 0) {
        return true;
    }
    if (messages == nullptr || count > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!is_valid_message(messages[i])) {
            return false;
        }
    }
    return true;
}

bool timeout_expired(std::chrono::steady_clock::time_point deadline, int64_t timeout_ms)
{
    return timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline;
}

std::chrono::steady_clock::time_point make_deadline(int64_t timeout_ms)
{
    if (timeout_ms < 0) {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
}

} // namespace

const char* galay_kernel_mpsc_channel_get_error(C_MpscChannelResultCode code)
{
    switch (code)
    {
    case C_MpscChannelSuccess:
        return "success";
    case C_MpscChannelParameterInvalid:
        return "parameter invalid";
    case C_MpscChannelMemoryAllocFailed:
        return "memory allocation failed";
    case C_MpscChannelIOFailed:
        return "io failed";
    case C_MpscChannelOperationInvalid:
        return "operation invalid";
    case C_MpscChannelTimeout:
        return "timeout";
    }
    return "unknown mpsc channel error";
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_create(galay_kernel_mpsc_channel_t* c_channel)
{
    if (c_channel == nullptr)
    {
        return C_MpscChannelParameterInvalid;
    }

    c_channel->channel = nullptr;
    auto* channel = new (std::nothrow) CppMpscChannel();
    if (channel == nullptr)
    {
        return C_MpscChannelMemoryAllocFailed;
    }

    c_channel->channel = channel;
    return C_MpscChannelSuccess;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_destroy(galay_kernel_mpsc_channel_t* c_channel)
{
    if (c_channel == nullptr)
    {
        return C_MpscChannelParameterInvalid;
    }

    delete to_cpp_channel(c_channel);
    c_channel->channel = nullptr;
    return C_MpscChannelSuccess;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_send(
    galay_kernel_mpsc_channel_t* c_channel,
    const C_MpscChannelMessage* message)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        message == nullptr || !is_valid_message(*message))
    {
        return C_MpscChannelParameterInvalid;
    }

    C_MpscChannelMessage copy = *message;
    return to_cpp_channel(c_channel)->send(std::move(copy))
        ? C_MpscChannelSuccess
        : C_MpscChannelIOFailed;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_send_batch(
    galay_kernel_mpsc_channel_t* c_channel,
    const C_MpscChannelMessage* messages,
    size_t count)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || !is_valid_batch(messages, count))
    {
        return C_MpscChannelParameterInvalid;
    }
    for (size_t i = 0; i < count; ++i) {
        C_MpscChannelResultCode sent = galay_kernel_mpsc_channel_send(c_channel, &messages[i]);
        if (sent != C_MpscChannelSuccess) {
            return sent;
        }
    }
    return C_MpscChannelSuccess;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_try_recv(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* message)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || message == nullptr)
    {
        return C_MpscChannelParameterInvalid;
    }

    auto received = to_cpp_channel(c_channel)->tryRecv();
    if (!received)
    {
        *message = C_MpscChannelMessage{};
        return C_MpscChannelTimeout;
    }

    *message = std::move(*received);
    return C_MpscChannelSuccess;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_try_recv_batch(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* messages,
    size_t max_count,
    size_t* out_count)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        messages == nullptr || max_count == 0 || out_count == nullptr)
    {
        return C_MpscChannelParameterInvalid;
    }

    *out_count = 0;
    auto received = to_cpp_channel(c_channel)->tryRecvBatch(max_count);
    if (!received)
    {
        return C_MpscChannelTimeout;
    }

    for (size_t i = 0; i < received->size(); ++i)
    {
        messages[i] = std::move((*received)[i]);
    }
    *out_count = received->size();
    return C_MpscChannelSuccess;
}

C_IOResult galay_kernel_mpsc_channel_recv(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* message,
    int64_t timeout_ms)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || message == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        *message = C_MpscChannelMessage{};
        return make_result(C_IOResultTimeout);
    }

    const auto deadline = make_deadline(timeout_ms);
    for (;;) {
        C_MpscChannelResultCode received = galay_kernel_mpsc_channel_try_recv(c_channel, message);
        if (received == C_MpscChannelSuccess) {
            return make_result(C_IOResultOk);
        }
        if (received != C_MpscChannelTimeout) {
            return make_result(C_IOResultInvalid);
        }
        if (timeout_expired(deadline, timeout_ms)) {
            *message = C_MpscChannelMessage{};
            return make_result(C_IOResultTimeout);
        }
        C_IOResult yielded = galay_coro_yield();
        if (yielded.code != C_IOResultOk) {
            return yielded.code == C_IOResultInvalid
                ? make_result(C_IOResultInvalid)
                : make_result(C_IOResultError, yielded.sys_errno);
        }
    }
}

C_IOResult galay_kernel_mpsc_channel_recv_batch(
    galay_kernel_mpsc_channel_t* c_channel,
    C_MpscChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        messages == nullptr || max_count == 0 || out_count == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    *out_count = 0;
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    const auto deadline = make_deadline(timeout_ms);
    for (;;) {
        C_MpscChannelResultCode received =
            galay_kernel_mpsc_channel_try_recv_batch(c_channel, messages, max_count, out_count);
        if (received == C_MpscChannelSuccess) {
            return make_result(C_IOResultOk);
        }
        if (received != C_MpscChannelTimeout) {
            return make_result(C_IOResultInvalid);
        }
        if (timeout_expired(deadline, timeout_ms)) {
            *out_count = 0;
            return make_result(C_IOResultTimeout);
        }
        C_IOResult yielded = galay_coro_yield();
        if (yielded.code != C_IOResultOk) {
            return yielded.code == C_IOResultInvalid
                ? make_result(C_IOResultInvalid)
                : make_result(C_IOResultError, yielded.sys_errno);
        }
    }
}

size_t galay_kernel_mpsc_channel_size(const galay_kernel_mpsc_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr)
    {
        return 0;
    }

    return to_cpp_channel(c_channel)->size();
}

bool galay_kernel_mpsc_channel_empty(const galay_kernel_mpsc_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr)
    {
        return true;
    }

    return to_cpp_channel(c_channel)->empty();
}
