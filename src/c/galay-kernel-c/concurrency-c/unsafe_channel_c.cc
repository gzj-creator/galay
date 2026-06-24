#include "unsafe_channel_c.h"

#include "../../../cpp/galay-kernel/common/error.h"
#include "../../../cpp/galay-kernel/concurrency/unsafe_channel.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace
{

using CppUnsafeChannel = galay::kernel::UnsafeChannel<C_UnsafeChannelMessage>;

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return static_cast<galay::kernel::Runtime*>(runtime->runtime);
}

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

C_UnsafeChannelResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kTimeout))
    {
        return C_UnsafeChannelTimeout;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid))
    {
        return C_UnsafeChannelParameterInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady))
    {
        return C_UnsafeChannelOperationInvalid;
    }
    return C_UnsafeChannelIOFailed;
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

bool timeout_fits_chrono(uint64_t timeout_ms)
{
    using Rep = std::chrono::milliseconds::rep;
    if constexpr (std::numeric_limits<Rep>::is_signed)
    {
        return timeout_ms <= static_cast<uint64_t>(std::numeric_limits<Rep>::max());
    }
    return timeout_ms <= std::numeric_limits<Rep>::max();
}

std::chrono::milliseconds to_timeout(uint64_t timeout_ms)
{
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms));
}

C_UnsafeChannelResultCode make_message_batch(
    const C_UnsafeChannelMessage* messages,
    size_t count,
    std::vector<C_UnsafeChannelMessage>* batch)
{
    try
    {
        batch->reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            batch->push_back(messages[i]);
        }
    }
    catch (const std::bad_alloc&)
    {
        return C_UnsafeChannelMemoryAllocFailed;
    }
    catch (...)
    {
        return C_UnsafeChannelMemoryAllocFailed;
    }
    return C_UnsafeChannelSuccess;
}

void set_single_success_result(
    galay_kernel_unsafe_channel_recv_result_t* result,
    C_UnsafeChannelMessage&& message)
{
    result->code = C_UnsafeChannelSuccess;
    result->message = message;
    result->messages = nullptr;
    result->count = 0;
}

void set_batch_success_result(
    galay_kernel_unsafe_channel_recv_result_t* result,
    std::vector<C_UnsafeChannelMessage>& messages)
{
    result->code = C_UnsafeChannelSuccess;
    result->message = C_UnsafeChannelMessage{};
    result->messages = messages.data();
    result->count = messages.size();
}

void set_error_result(
    galay_kernel_unsafe_channel_recv_result_t* result,
    C_UnsafeChannelResultCode code)
{
    result->code = code;
    result->message = C_UnsafeChannelMessage{};
    result->messages = nullptr;
    result->count = 0;
}

galay::kernel::Task<void> c_api_recv(
    CppUnsafeChannel* channel,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recv();
    galay_kernel_unsafe_channel_recv_result_t result{};
    if (received)
    {
        set_single_success_result(&result, std::move(*received));
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_timeout(
    CppUnsafeChannel* channel,
    std::chrono::milliseconds timeout,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recv().timeout(timeout);
    galay_kernel_unsafe_channel_recv_result_t result{};
    if (received)
    {
        set_single_success_result(&result, std::move(*received));
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_batch(
    CppUnsafeChannel* channel,
    size_t max_count,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recvBatch(max_count);
    galay_kernel_unsafe_channel_recv_result_t result{};
    if (received)
    {
        set_batch_success_result(&result, *received);
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_batch_timeout(
    CppUnsafeChannel* channel,
    size_t max_count,
    std::chrono::milliseconds timeout,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recvBatch(max_count).timeout(timeout);
    galay_kernel_unsafe_channel_recv_result_t result{};
    if (received)
    {
        set_batch_success_result(&result, *received);
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_batched(
    CppUnsafeChannel* channel,
    size_t limit,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recvBatched(limit);
    galay_kernel_unsafe_channel_recv_result_t result{};
    if (received)
    {
        set_batch_success_result(&result, *received);
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_batched_timeout(
    CppUnsafeChannel* channel,
    size_t limit,
    std::chrono::milliseconds timeout,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recvBatched(limit).timeout(timeout);
    galay_kernel_unsafe_channel_recv_result_t result{};
    if (received)
    {
        set_batch_success_result(&result, *received);
    }
    else
    {
        const auto code = from_cpp_io_error(received.error());
        if (code == C_UnsafeChannelTimeout)
        {
            const size_t pending = channel->size();
            if (pending > 0)
            {
                auto partial = channel->tryRecvBatch(pending);
                if (partial && !partial->empty())
                {
                    set_batch_success_result(&result, *partial);
                    callback(&result, ctx);
                    co_return;
                }
            }
        }
        set_error_result(&result, code);
    }
    callback(&result, ctx);
    co_return;
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
    case C_UnsafeChannelRuntimeNotRunning:
        return "runtime not running";
    case C_UnsafeChannelRuntimeSpawnFailed:
        return "runtime spawn failed";
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
    if (count == 0)
    {
        return C_UnsafeChannelSuccess;
    }

    std::vector<C_UnsafeChannelMessage> batch;
    auto made_batch = make_message_batch(messages, count, &batch);
    if (made_batch != C_UnsafeChannelSuccess)
    {
        return made_batch;
    }

    return to_cpp_channel(c_channel)->sendBatch(std::move(batch))
        ? C_UnsafeChannelSuccess
        : C_UnsafeChannelIOFailed;
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

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        callback == nullptr)
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UnsafeChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(c_api_recv(to_cpp_channel(c_channel), callback, ctx));
    return spawned ? C_UnsafeChannelSuccess : C_UnsafeChannelRuntimeSpawnFailed;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    uint64_t timeout_ms,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UnsafeChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_timeout(to_cpp_channel(c_channel), to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_UnsafeChannelSuccess : C_UnsafeChannelRuntimeSpawnFailed;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batch(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t max_count,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        max_count == 0 || callback == nullptr)
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UnsafeChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_batch(to_cpp_channel(c_channel), max_count, callback, ctx));
    return spawned ? C_UnsafeChannelSuccess : C_UnsafeChannelRuntimeSpawnFailed;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batch_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t max_count,
    uint64_t timeout_ms,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        max_count == 0 || callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UnsafeChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_batch_timeout(to_cpp_channel(c_channel), max_count, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_UnsafeChannelSuccess : C_UnsafeChannelRuntimeSpawnFailed;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batched(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t limit,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        limit == 0 || callback == nullptr)
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UnsafeChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_batched(to_cpp_channel(c_channel), limit, callback, ctx));
    return spawned ? C_UnsafeChannelSuccess : C_UnsafeChannelRuntimeSpawnFailed;
}

C_UnsafeChannelResultCode galay_kernel_unsafe_channel_recv_batched_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_unsafe_channel_t* c_channel,
    size_t limit,
    uint64_t timeout_ms,
    galay_kernel_unsafe_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        limit == 0 || callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_UnsafeChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_UnsafeChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_batched_timeout(to_cpp_channel(c_channel), limit, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_UnsafeChannelSuccess : C_UnsafeChannelRuntimeSpawnFailed;
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
