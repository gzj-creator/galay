#include "unsafe_channel_c.h"

#include "../../../cpp/galay-kernel/concurrency/unsafe_channel.h"
#include "../coro-c/coro_task_c.h"
#include "../coro-c/coro_wait_c.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

namespace
{

using CppUnsafeChannel = galay::kernel::UnsafeChannel<C_UnsafeChannelMessage>;

CppUnsafeChannel* to_cpp_channel(galay_kernel_unsafe_channel_t* channel)
{
    return static_cast<CppUnsafeChannel*>(channel->channel);
}

const CppUnsafeChannel* to_cpp_channel(const galay_kernel_unsafe_channel_t* channel)
{
    return static_cast<const CppUnsafeChannel*>(channel->channel);
}

bool is_valid_wake_mode(C_UnsafeChannelWakeMode wake_mode)
{
    return wake_mode == C_UnsafeChannelWakeModeInline ||
           wake_mode == C_UnsafeChannelWakeModeDeferred;
}

galay::kernel::UnsafeChannelWakeMode to_cpp_wake_mode(C_UnsafeChannelWakeMode wake_mode)
{
    switch (wake_mode)
    {
    case C_UnsafeChannelWakeModeInline:
        return galay::kernel::UnsafeChannelWakeMode::Inline;
    case C_UnsafeChannelWakeModeDeferred:
        return galay::kernel::UnsafeChannelWakeMode::Deferred;
    }
    return galay::kernel::UnsafeChannelWakeMode::Inline;
}

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

bool is_valid_message(const C_UnsafeChannelMessage& message)
{
    return message.data != nullptr || message.size == 0;
}

bool is_valid_batch(const C_UnsafeChannelMessage* messages, size_t count)
{
    if (count == 0)
    {
        return true;
    }
    if (messages == nullptr || count > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (!is_valid_message(messages[i]))
        {
            return false;
        }
    }
    return true;
}

std::chrono::steady_clock::time_point make_deadline(int64_t timeout_ms)
{
    if (timeout_ms < 0)
    {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
}

bool timeout_expired(std::chrono::steady_clock::time_point deadline, int64_t timeout_ms)
{
    return timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline;
}

C_IOResult wait_next_poll(C_CoroWaitRequest* request,
                          std::chrono::steady_clock::time_point deadline,
                          int64_t timeout_ms)
{
    uint64_t generation = 0;
    C_IOResult prepared = galay_coro_wait_request_prepare(request, &generation);
    if (prepared.code != C_IOResultOk)
    {
        return make_result(C_IOResultInvalid);
    }

    int64_t poll_timeout_ms = 1;
    if (timeout_ms >= 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return make_result(C_IOResultTimeout);
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        poll_timeout_ms = remaining <= 1 ? 1 : 1;
    }

    C_IOResult waited = galay_coro_wait(request, poll_timeout_ms);
    if (waited.code == C_IOResultTimeout)
    {
        return make_result(C_IOResultOk);
    }
    return waited;
}

C_IOResult destroy_request_and_return(C_CoroWaitRequest* request, C_IOResult result)
{
    if (request != nullptr && request->request != nullptr)
    {
        C_IOResult destroyed = galay_coro_wait_request_destroy(request);
        if (destroyed.code != C_IOResultOk && result.code == C_IOResultOk)
        {
            return make_result(C_IOResultInvalid);
        }
    }
    return result;
}

} // namespace

const char* galay_kernel_unsafe_channel_get_error(C_UnsafeChannelResultCode code)
{
    switch (code)
    {
    case C_UnsafeChannelSuccess:
        return "success";
    case C_UnsafeChannelParameterInvalid:
        return "parameter invalid";
    case C_UnsafeChannelMemoryAllocFailed:
        return "memory allocation failed";
    case C_UnsafeChannelIOFailed:
        return "io failed";
    case C_UnsafeChannelOperationInvalid:
        return "operation invalid";
    case C_UnsafeChannelTimeout:
        return "timeout";
    }
    return "unknown unsafe channel error";
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_create(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelWakeMode wake_mode)
{
    if (c_channel == nullptr || !is_valid_wake_mode(wake_mode))
    {
        return C_UnsafeChannelParameterInvalid;
    }

    c_channel->channel = nullptr;
    auto* channel = new (std::nothrow) CppUnsafeChannel(to_cpp_wake_mode(wake_mode));
    if (channel == nullptr)
    {
        return C_UnsafeChannelMemoryAllocFailed;
    }

    c_channel->channel = channel;
    return C_UnsafeChannelSuccess;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_destroy(
    galay_kernel_unsafe_channel_t* c_channel)
{
    if (c_channel == nullptr)
    {
        return C_UnsafeChannelParameterInvalid;
    }

    delete to_cpp_channel(c_channel);
    c_channel->channel = nullptr;
    return C_UnsafeChannelSuccess;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_send(
    galay_kernel_unsafe_channel_t* c_channel,
    const C_UnsafeChannelMessage* message)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        message == nullptr || !is_valid_message(*message))
    {
        return C_UnsafeChannelParameterInvalid;
    }

    C_UnsafeChannelMessage copy = *message;
    return to_cpp_channel(c_channel)->send(std::move(copy))
        ? C_UnsafeChannelSuccess
        : C_UnsafeChannelIOFailed;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_send_batch(
    galay_kernel_unsafe_channel_t* c_channel,
    const C_UnsafeChannelMessage* messages,
    size_t count)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || !is_valid_batch(messages, count))
    {
        return C_UnsafeChannelParameterInvalid;
    }

    for (size_t i = 0; i < count; ++i)
    {
        C_UnsafeChannelResultCode sent = galay_kernel_unsafe_channel_send(c_channel, &messages[i]);
        if (sent != C_UnsafeChannelSuccess)
        {
            return sent;
        }
    }
    return C_UnsafeChannelSuccess;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_try_recv(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelMessage* message)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || message == nullptr)
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto received = to_cpp_channel(c_channel)->tryRecv();
    if (!received)
    {
        *message = C_UnsafeChannelMessage{};
        return C_UnsafeChannelTimeout;
    }

    *message = std::move(*received);
    return C_UnsafeChannelSuccess;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_try_recv_batch(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelMessage* messages,
    size_t max_count,
    size_t* out_count)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        messages == nullptr || max_count == 0 || out_count == nullptr)
    {
        return C_UnsafeChannelParameterInvalid;
    }

    *out_count = 0;
    auto received = to_cpp_channel(c_channel)->tryRecvBatch(max_count);
    if (!received)
    {
        return C_UnsafeChannelTimeout;
    }

    for (size_t i = 0; i < received->size(); ++i)
    {
        messages[i] = std::move((*received)[i]);
    }
    *out_count = received->size();
    return C_UnsafeChannelSuccess;
}

C_IOResult galay_kernel_unsafe_channel_recv(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelMessage* message,
    int64_t timeout_ms)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || message == nullptr)
    {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0)
    {
        *message = C_UnsafeChannelMessage{};
        return make_result(C_IOResultTimeout);
    }

    C_CoroWaitRequest poll_request{};
    C_IOResult created = galay_coro_wait_request_create(&poll_request);
    if (created.code != C_IOResultOk)
    {
        return created.code == C_IOResultInvalid
            ? make_result(C_IOResultInvalid)
            : make_result(C_IOResultError, created.sys_errno);
    }
    const auto deadline = make_deadline(timeout_ms);
    for (;;)
    {
        C_UnsafeChannelResultCode received =
            galay_kernel_unsafe_channel_try_recv(c_channel, message);
        if (received == C_UnsafeChannelSuccess)
        {
            return destroy_request_and_return(&poll_request, make_result(C_IOResultOk));
        }
        if (received != C_UnsafeChannelTimeout)
        {
            return destroy_request_and_return(&poll_request, make_result(C_IOResultInvalid));
        }
        if (timeout_expired(deadline, timeout_ms))
        {
            *message = C_UnsafeChannelMessage{};
            return destroy_request_and_return(&poll_request, make_result(C_IOResultTimeout));
        }
        C_IOResult parked = wait_next_poll(&poll_request, deadline, timeout_ms);
        if (parked.code != C_IOResultOk)
        {
            return destroy_request_and_return(&poll_request, parked);
        }
    }
}

C_IOResult galay_kernel_unsafe_channel_recv_batch(
    galay_kernel_unsafe_channel_t* c_channel,
    C_UnsafeChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms)
{
    if (c_channel == nullptr || c_channel->channel == nullptr ||
        messages == nullptr || max_count == 0 || out_count == nullptr)
    {
        return make_result(C_IOResultInvalid);
    }
    *out_count = 0;
    if (timeout_ms == 0)
    {
        return make_result(C_IOResultTimeout);
    }

    C_CoroWaitRequest poll_request{};
    C_IOResult created = galay_coro_wait_request_create(&poll_request);
    if (created.code != C_IOResultOk)
    {
        return created.code == C_IOResultInvalid
            ? make_result(C_IOResultInvalid)
            : make_result(C_IOResultError, created.sys_errno);
    }
    const auto deadline = make_deadline(timeout_ms);
    for (;;)
    {
        C_UnsafeChannelResultCode received =
            galay_kernel_unsafe_channel_try_recv_batch(c_channel, messages, max_count, out_count);
        if (received == C_UnsafeChannelSuccess)
        {
            return destroy_request_and_return(&poll_request, make_result(C_IOResultOk));
        }
        if (received != C_UnsafeChannelTimeout)
        {
            return destroy_request_and_return(&poll_request, make_result(C_IOResultInvalid));
        }
        if (timeout_expired(deadline, timeout_ms))
        {
            *out_count = 0;
            return destroy_request_and_return(&poll_request, make_result(C_IOResultTimeout));
        }
        C_IOResult parked = wait_next_poll(&poll_request, deadline, timeout_ms);
        if (parked.code != C_IOResultOk)
        {
            return destroy_request_and_return(&poll_request, parked);
        }
    }
}

C_IOResult galay_kernel_unsafe_channel_recv_batched(
    galay_kernel_unsafe_channel_t* c_channel,
    size_t limit,
    C_UnsafeChannelMessage* messages,
    size_t max_count,
    size_t* out_count,
    int64_t timeout_ms)
{
    if (c_channel == nullptr || c_channel->channel == nullptr || limit == 0 ||
        messages == nullptr || max_count == 0 || out_count == nullptr)
    {
        return make_result(C_IOResultInvalid);
    }
    *out_count = 0;

    if (timeout_ms == 0)
    {
        if (galay_kernel_unsafe_channel_size(c_channel) > 0)
        {
            C_UnsafeChannelResultCode received =
                galay_kernel_unsafe_channel_try_recv_batch(c_channel, messages, max_count, out_count);
            return received == C_UnsafeChannelSuccess
                ? make_result(C_IOResultOk)
                : make_result(C_IOResultInvalid);
        }
        return make_result(C_IOResultTimeout);
    }

    C_CoroWaitRequest poll_request{};
    C_IOResult created = galay_coro_wait_request_create(&poll_request);
    if (created.code != C_IOResultOk)
    {
        return created.code == C_IOResultInvalid
            ? make_result(C_IOResultInvalid)
            : make_result(C_IOResultError, created.sys_errno);
    }
    const auto deadline = make_deadline(timeout_ms);
    for (;;)
    {
        if (galay_kernel_unsafe_channel_size(c_channel) >= limit)
        {
            C_UnsafeChannelResultCode received =
                galay_kernel_unsafe_channel_try_recv_batch(c_channel, messages, max_count, out_count);
            return received == C_UnsafeChannelSuccess
                ? destroy_request_and_return(&poll_request, make_result(C_IOResultOk))
                : destroy_request_and_return(&poll_request, make_result(C_IOResultInvalid));
        }
        if (timeout_expired(deadline, timeout_ms))
        {
            if (galay_kernel_unsafe_channel_size(c_channel) > 0)
            {
                C_UnsafeChannelResultCode received =
                    galay_kernel_unsafe_channel_try_recv_batch(c_channel, messages, max_count, out_count);
                return received == C_UnsafeChannelSuccess
                    ? destroy_request_and_return(&poll_request, make_result(C_IOResultOk))
                    : destroy_request_and_return(&poll_request, make_result(C_IOResultInvalid));
            }
            *out_count = 0;
            return destroy_request_and_return(&poll_request, make_result(C_IOResultTimeout));
        }
        C_IOResult parked = wait_next_poll(&poll_request, deadline, timeout_ms);
        if (parked.code != C_IOResultOk)
        {
            return destroy_request_and_return(&poll_request, parked);
        }
    }
}

size_t galay_kernel_unsafe_channel_size(const galay_kernel_unsafe_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr)
    {
        return 0;
    }

    return to_cpp_channel(c_channel)->size();
}

bool galay_kernel_unsafe_channel_empty(const galay_kernel_unsafe_channel_t* c_channel)
{
    if (c_channel == nullptr || c_channel->channel == nullptr)
    {
        return true;
    }

    return to_cpp_channel(c_channel)->empty();
}
