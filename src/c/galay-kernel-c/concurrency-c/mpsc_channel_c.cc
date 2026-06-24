#include "mpsc_channel_c.h"

#include "../../../cpp/galay-kernel/common/error.h"
#include "../../../cpp/galay-kernel/concurrency/mpsc_channel.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace
{

using CppMpscChannel = galay::kernel::MpscChannel<C_MpscChannelMessage>;

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return static_cast<galay::kernel::Runtime*>(runtime->runtime);
}

CppMpscChannel* to_cpp_channel(galay_kernel_mpsc_channel_t* channel)
{
    return static_cast<CppMpscChannel*>(channel->channel);
}

const CppMpscChannel* to_cpp_channel(const galay_kernel_mpsc_channel_t* channel)
{
    return static_cast<const CppMpscChannel*>(channel->channel);
}

C_MpscChannelResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kTimeout))
    {
        return C_MpscChannelTimeout;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid))
    {
        return C_MpscChannelParameterInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady))
    {
        return C_MpscChannelOperationInvalid;
    }
    return C_MpscChannelIOFailed;
}

bool is_valid_message(const C_MpscChannelMessage& message)
{
    return message.data != nullptr || message.size == 0;
}

bool is_valid_batch(const C_MpscChannelMessage* messages, size_t count)
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

C_MpscChannelResultCode make_message_batch(
    const C_MpscChannelMessage* messages,
    size_t count,
    std::vector<C_MpscChannelMessage>* batch)
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
        return C_MpscChannelMemoryAllocFailed;
    }
    catch (...)
    {
        return C_MpscChannelMemoryAllocFailed;
    }
    return C_MpscChannelSuccess;
}

void set_single_success_result(
    galay_kernel_mpsc_channel_recv_result_t* result,
    C_MpscChannelMessage&& message)
{
    result->code = C_MpscChannelSuccess;
    result->message = message;
    result->messages = nullptr;
    result->count = 0;
}

void set_error_result(
    galay_kernel_mpsc_channel_recv_result_t* result,
    C_MpscChannelResultCode code)
{
    result->code = code;
    result->message = C_MpscChannelMessage{};
    result->messages = nullptr;
    result->count = 0;
}

galay::kernel::Task<void> c_api_recv(
    CppMpscChannel* channel,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recv();
    galay_kernel_mpsc_channel_recv_result_t result{};
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
    CppMpscChannel* channel,
    std::chrono::milliseconds timeout,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recv().timeout(timeout);
    galay_kernel_mpsc_channel_recv_result_t result{};
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
    CppMpscChannel* channel,
    size_t max_count,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recvBatch(max_count);
    galay_kernel_mpsc_channel_recv_result_t result{};
    if (received)
    {
        result.code = C_MpscChannelSuccess;
        result.message = C_MpscChannelMessage{};
        result.messages = received->data();
        result.count = received->size();
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_recv_batch_timeout(
    CppMpscChannel* channel,
    size_t max_count,
    std::chrono::milliseconds timeout,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    auto received = co_await channel->recvBatch(max_count).timeout(timeout);
    galay_kernel_mpsc_channel_recv_result_t result{};
    if (received)
    {
        result.code = C_MpscChannelSuccess;
        result.message = C_MpscChannelMessage{};
        result.messages = received->data();
        result.count = received->size();
    }
    else
    {
        set_error_result(&result, from_cpp_io_error(received.error()));
    }
    callback(&result, ctx);
    co_return;
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
    case C_MpscChannelRuntimeNotRunning:
        return "runtime not running";
    case C_MpscChannelRuntimeSpawnFailed:
        return "runtime spawn failed";
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
    if (count == 0)
    {
        return C_MpscChannelSuccess;
    }

    std::vector<C_MpscChannelMessage> batch;
    auto made_batch = make_message_batch(messages, count, &batch);
    if (made_batch != C_MpscChannelSuccess)
    {
        return made_batch;
    }

    return to_cpp_channel(c_channel)->sendBatch(std::move(batch))
        ? C_MpscChannelSuccess
        : C_MpscChannelIOFailed;
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

C_MpscChannelResultCode galay_kernel_mpsc_channel_recv(
    galay_kernel_runtime_t* runtime,
    galay_kernel_mpsc_channel_t* c_channel,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        callback == nullptr)
    {
        return C_MpscChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_MpscChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(c_api_recv(to_cpp_channel(c_channel), callback, ctx));
    return spawned ? C_MpscChannelSuccess : C_MpscChannelRuntimeSpawnFailed;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_recv_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_mpsc_channel_t* c_channel,
    uint64_t timeout_ms,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_MpscChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_MpscChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_timeout(to_cpp_channel(c_channel), to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_MpscChannelSuccess : C_MpscChannelRuntimeSpawnFailed;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_recv_batch(
    galay_kernel_runtime_t* runtime,
    galay_kernel_mpsc_channel_t* c_channel,
    size_t max_count,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        max_count == 0 || callback == nullptr)
    {
        return C_MpscChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_MpscChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_batch(to_cpp_channel(c_channel), max_count, callback, ctx));
    return spawned ? C_MpscChannelSuccess : C_MpscChannelRuntimeSpawnFailed;
}

C_MpscChannelResultCode galay_kernel_mpsc_channel_recv_batch_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_mpsc_channel_t* c_channel,
    size_t max_count,
    uint64_t timeout_ms,
    galay_kernel_mpsc_channel_recv_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_channel == nullptr || c_channel->channel == nullptr ||
        max_count == 0 || callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_MpscChannelParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_MpscChannelRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(
        c_api_recv_batch_timeout(to_cpp_channel(c_channel), max_count, to_timeout(timeout_ms), callback, ctx));
    return spawned ? C_MpscChannelSuccess : C_MpscChannelRuntimeSpawnFailed;
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
