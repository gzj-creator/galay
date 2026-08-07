#ifndef GALAY_KERNEL_CHANNEL_C_IMPL_H
#define GALAY_KERNEL_CHANNEL_C_IMPL_H

#include "../../coro-c/coro_wait_c.h"

#include <chrono>
#include <limits>
#include <new>
#include <utility>

namespace galay::c_api::channel
{

inline C_IOResult makeResult(C_IOResultCode code,
                             size_t bytes = 0,
                             int sysErrno = 0) noexcept
{
    return C_IOResult{code, sysErrno, bytes, 0, nullptr};
}

inline bool validMessage(const C_ChannelMessage& message) noexcept
{
    return message.data != nullptr || message.size == 0;
}

inline bool validBatch(const C_ChannelMessage* messages, size_t count) noexcept
{
    if (count == 0) {
        return true;
    }
    if (messages == nullptr) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!validMessage(messages[index])) {
            return false;
        }
    }
    return true;
}

inline bool validCapacity(size_t capacity) noexcept
{
    constexpr size_t kMaxPowerOfTwo =
        size_t{1} << (std::numeric_limits<size_t>::digits - 1);
    return capacity <= kMaxPowerOfTwo;
}

inline std::chrono::steady_clock::time_point makeDeadline(
    int64_t timeoutMs) noexcept
{
    if (timeoutMs < 0) {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - now);
    if (timeoutMs >= maximum.count()) {
        return std::chrono::steady_clock::time_point::max();
    }
    return now + std::chrono::milliseconds(timeoutMs);
}

inline bool timeoutExpired(std::chrono::steady_clock::time_point deadline,
                           int64_t timeoutMs) noexcept
{
    return timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline;
}

inline C_IOResult suspendBeforeRetry() noexcept
{
    C_IOResult slept = galay_coro_sleep(1);
    if (slept.code == C_IOResultOk ||
        slept.code == C_IOResultCancelled ||
        slept.code == C_IOResultInvalid) {
        return slept;
    }
    return makeResult(C_IOResultError, 0, slept.sys_errno);
}

template <typename Channel, typename Handle>
Channel* unwrap(Handle* handle) noexcept
{
    return static_cast<Channel*>(handle->channel);
}

template <typename Channel, typename Handle>
const Channel* unwrap(const Handle* handle) noexcept
{
    return static_cast<const Channel*>(handle->channel);
}

template <typename Channel>
bool isValid(const Channel& channel) noexcept
{
    if constexpr (requires(const Channel& value) { value.isValid(); }) {
        return channel.isValid();
    }
    return true;
}

template <typename Channel, typename Handle>
C_ChannelResultCode destroy(Handle* handle) noexcept
{
    if (handle == nullptr) {
        return C_ChannelParameterInvalid;
    }
    delete unwrap<Channel>(handle);
    handle->channel = nullptr;
    return C_ChannelSuccess;
}

template <typename Channel, typename Token, typename Handle,
          typename TokenHandle, typename MakeToken>
C_ChannelResultCode createToken(Handle* handle,
                                TokenHandle* token,
                                MakeToken makeToken) noexcept
{
    if (handle == nullptr || handle->channel == nullptr || token == nullptr) {
        return C_ChannelParameterInvalid;
    }
    token->token = nullptr;
    token->channel = nullptr;
    Channel* channel = unwrap<Channel>(handle);
    auto* nativeToken = new (std::nothrow) Token(makeToken(*channel));
    if (nativeToken == nullptr) {
        return C_ChannelMemoryAllocFailed;
    }
    if (!nativeToken->valid()) {
        delete nativeToken;
        return channel->isClosed()
            ? C_ChannelClosed
            : C_ChannelMemoryAllocFailed;
    }
    token->token = nativeToken;
    token->channel = handle->channel;
    return C_ChannelSuccess;
}

template <typename Token, typename TokenHandle>
C_ChannelResultCode destroyToken(TokenHandle* token) noexcept
{
    if (token == nullptr) {
        return C_ChannelParameterInvalid;
    }
    delete static_cast<Token*>(token->token);
    token->token = nullptr;
    token->channel = nullptr;
    return C_ChannelSuccess;
}

template <typename Channel, typename Token, typename Handle,
          typename TokenHandle>
C_ChannelResultCode tryUnboundedSendWithToken(
    Handle* handle,
    TokenHandle* token,
    const C_ChannelMessage* message) noexcept
{
    if (handle == nullptr || handle->channel == nullptr || token == nullptr ||
        token->token == nullptr || token->channel != handle->channel ||
        message == nullptr || !validMessage(*message)) {
        return C_ChannelParameterInvalid;
    }
    Channel& channel = *unwrap<Channel>(handle);
    C_ChannelMessage copy = *message;
    if (channel.send(*static_cast<Token*>(token->token), std::move(copy))) {
        return C_ChannelSuccess;
    }
    return channel.isClosed() ? C_ChannelClosed : C_ChannelIOFailed;
}

template <typename Channel, typename Token, typename Handle,
          typename TokenHandle>
C_ChannelResultCode tryRecvWithToken(Handle* handle,
                                      TokenHandle* token,
                                      C_ChannelMessage* message) noexcept
{
    if (handle == nullptr || handle->channel == nullptr || token == nullptr ||
        token->token == nullptr || token->channel != handle->channel ||
        message == nullptr) {
        return C_ChannelParameterInvalid;
    }
    *message = C_ChannelMessage{};
    Channel& channel = *unwrap<Channel>(handle);
    auto received = channel.tryRecv(*static_cast<Token*>(token->token));
    if (received.has_value()) {
        *message = std::move(*received);
        return C_ChannelSuccess;
    }
    return channel.isClosed() ? C_ChannelClosed : C_ChannelEmpty;
}

template <typename Channel>
C_ChannelResultCode tryBoundedSend(Channel& channel,
                                   const C_ChannelMessage& message) noexcept
{
    C_ChannelMessage copy = message;
    if (channel.trySend(std::move(copy))) {
        return C_ChannelSuccess;
    }
    return channel.isClosed() ? C_ChannelClosed : C_ChannelFull;
}

template <typename Channel>
C_ChannelResultCode tryUnboundedSend(Channel& channel,
                                     const C_ChannelMessage& message) noexcept
{
    C_ChannelMessage copy = message;
    if (channel.send(std::move(copy))) {
        return C_ChannelSuccess;
    }
    if constexpr (requires { channel.isClosed(); }) {
        if (channel.isClosed()) {
            return C_ChannelClosed;
        }
    }
    return C_ChannelIOFailed;
}

template <typename Channel>
C_ChannelResultCode tryRecv(Channel& channel,
                            C_ChannelMessage& message) noexcept
{
    message = C_ChannelMessage{};
    auto received = channel.tryRecv();
    if (received.has_value()) {
        message = std::move(*received);
        return C_ChannelSuccess;
    }
    if constexpr (requires { channel.isClosed(); }) {
        if (channel.isClosed()) {
            return C_ChannelClosed;
        }
    }
    return C_ChannelEmpty;
}

template <typename Channel>
C_ChannelResultCode tryRecvBatch(Channel& channel,
                                 C_ChannelMessage* messages,
                                 size_t maxCount,
                                 size_t& outCount) noexcept
{
    outCount = 0;
    while (outCount < maxCount) {
        C_ChannelResultCode result = tryRecv(channel, messages[outCount]);
        if (result == C_ChannelSuccess) {
            ++outCount;
            continue;
        }
        return outCount == 0 ? result : C_ChannelSuccess;
    }
    return C_ChannelSuccess;
}

template <typename TryOperation>
C_IOResult waitForOne(TryOperation&& operation, int64_t timeoutMs) noexcept
{
    const auto deadline = makeDeadline(timeoutMs);
    for (;;) {
        C_ChannelResultCode result = operation();
        if (result == C_ChannelSuccess) {
            return makeResult(C_IOResultOk, 1);
        }
        if (result == C_ChannelClosed) {
            return makeResult(C_IOResultCancelled);
        }
        if (result != C_ChannelFull && result != C_ChannelEmpty) {
            return makeResult(C_IOResultInvalid);
        }
        if (timeoutMs == 0 || timeoutExpired(deadline, timeoutMs)) {
            return makeResult(C_IOResultTimeout);
        }
        C_IOResult suspended = suspendBeforeRetry();
        if (suspended.code != C_IOResultOk) {
            return suspended;
        }
    }
}

template <typename TryOperation>
C_IOResult waitForBatch(TryOperation&& operation,
                        size_t& outCount,
                        int64_t timeoutMs) noexcept
{
    const auto deadline = makeDeadline(timeoutMs);
    for (;;) {
        C_ChannelResultCode result = operation();
        if (result == C_ChannelSuccess) {
            return makeResult(C_IOResultOk, outCount);
        }
        if (result == C_ChannelClosed) {
            return makeResult(C_IOResultCancelled);
        }
        if (result != C_ChannelEmpty) {
            return makeResult(C_IOResultInvalid);
        }
        if (timeoutMs == 0 || timeoutExpired(deadline, timeoutMs)) {
            return makeResult(C_IOResultTimeout);
        }
        C_IOResult suspended = suspendBeforeRetry();
        if (suspended.code != C_IOResultOk) {
            return suspended;
        }
    }
}

} // namespace galay::c_api::channel

#define GALAY_DEFINE_BOUNDED_CHANNEL_C_API(PREFIX, HANDLE_TYPE, CHANNEL_TYPE) \
    C_ChannelResultCode PREFIX##_create(HANDLE_TYPE* handle, size_t capacity) \
    { \
        if (handle == nullptr || \
            !::galay::c_api::channel::validCapacity(capacity)) { \
            return C_ChannelParameterInvalid; \
        } \
        handle->channel = nullptr; \
        auto* channel = new (std::nothrow) CHANNEL_TYPE(capacity); \
        if (channel == nullptr) { \
            return C_ChannelMemoryAllocFailed; \
        } \
        if (!::galay::c_api::channel::isValid(*channel)) { \
            delete channel; \
            return C_ChannelMemoryAllocFailed; \
        } \
        handle->channel = channel; \
        return C_ChannelSuccess; \
    } \
    C_ChannelResultCode PREFIX##_destroy(HANDLE_TYPE* handle) \
    { \
        return ::galay::c_api::channel::destroy<CHANNEL_TYPE>(handle); \
    } \
    C_ChannelResultCode PREFIX##_try_send( \
        HANDLE_TYPE* handle, const C_ChannelMessage* message) \
    { \
        if (handle == nullptr || handle->channel == nullptr || \
            message == nullptr || \
            !::galay::c_api::channel::validMessage(*message)) { \
            return C_ChannelParameterInvalid; \
        } \
        return ::galay::c_api::channel::tryBoundedSend( \
            *::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle), *message); \
    } \
    C_ChannelResultCode PREFIX##_try_send_batch( \
        HANDLE_TYPE* handle, const C_ChannelMessage* messages, \
        size_t count, size_t* outCount) \
    { \
        if (outCount == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        *outCount = 0; \
        if (handle == nullptr || handle->channel == nullptr || \
            !::galay::c_api::channel::validBatch(messages, count)) { \
            return C_ChannelParameterInvalid; \
        } \
        for (; *outCount < count; ++*outCount) { \
            C_ChannelResultCode result = PREFIX##_try_send( \
                handle, &messages[*outCount]); \
            if (result != C_ChannelSuccess) { \
                return result; \
            } \
        } \
        return C_ChannelSuccess; \
    } \
    C_IOResult PREFIX##_send(HANDLE_TYPE* handle, \
                             const C_ChannelMessage* message, \
                             int64_t timeoutMs) \
    { \
        return ::galay::c_api::channel::waitForOne( \
            [handle, message]() noexcept { \
                return PREFIX##_try_send(handle, message); \
            }, timeoutMs); \
    } \
    C_ChannelResultCode PREFIX##_try_recv( \
        HANDLE_TYPE* handle, C_ChannelMessage* message) \
    { \
        if (handle == nullptr || handle->channel == nullptr || \
            message == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        return ::galay::c_api::channel::tryRecv( \
            *::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle), *message); \
    } \
    C_ChannelResultCode PREFIX##_try_recv_batch( \
        HANDLE_TYPE* handle, C_ChannelMessage* messages, \
        size_t maxCount, size_t* outCount) \
    { \
        if (handle == nullptr || handle->channel == nullptr || \
            messages == nullptr || maxCount == 0 || outCount == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        return ::galay::c_api::channel::tryRecvBatch( \
            *::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle), \
            messages, maxCount, *outCount); \
    } \
    C_IOResult PREFIX##_recv(HANDLE_TYPE* handle, \
                             C_ChannelMessage* message, \
                             int64_t timeoutMs) \
    { \
        return ::galay::c_api::channel::waitForOne( \
            [handle, message]() noexcept { \
                return PREFIX##_try_recv(handle, message); \
            }, timeoutMs); \
    } \
    C_IOResult PREFIX##_recv_batch( \
        HANDLE_TYPE* handle, C_ChannelMessage* messages, \
        size_t maxCount, size_t* outCount, int64_t timeoutMs) \
    { \
        if (outCount == nullptr) { \
            return ::galay::c_api::channel::makeResult(C_IOResultInvalid); \
        } \
        *outCount = 0; \
        return ::galay::c_api::channel::waitForBatch( \
            [handle, messages, maxCount, outCount]() noexcept { \
                return PREFIX##_try_recv_batch( \
                    handle, messages, maxCount, outCount); \
            }, *outCount, timeoutMs); \
    } \
    C_ChannelResultCode PREFIX##_close(HANDLE_TYPE* handle) \
    { \
        if (handle == nullptr || handle->channel == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->close(); \
        return C_ChannelSuccess; \
    } \
    bool PREFIX##_is_closed(const HANDLE_TYPE* handle) \
    { \
        return handle == nullptr || handle->channel == nullptr || \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->isClosed(); \
    } \
    size_t PREFIX##_capacity(const HANDLE_TYPE* handle) \
    { \
        return handle == nullptr || handle->channel == nullptr ? 0 : \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->capacity(); \
    } \
    size_t PREFIX##_size(const HANDLE_TYPE* handle) \
    { \
        return handle == nullptr || handle->channel == nullptr ? 0 : \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->size(); \
    } \
    bool PREFIX##_empty(const HANDLE_TYPE* handle) \
    { \
        return handle == nullptr || handle->channel == nullptr || \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->empty(); \
    } \
    bool PREFIX##_full(const HANDLE_TYPE* handle) \
    { \
        return handle != nullptr && handle->channel != nullptr && \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->full(); \
    }

#define GALAY_DEFINE_UNBOUNDED_CHANNEL_C_API(PREFIX, HANDLE_TYPE, CHANNEL_TYPE) \
    C_ChannelResultCode PREFIX##_destroy(HANDLE_TYPE* handle) \
    { \
        return ::galay::c_api::channel::destroy<CHANNEL_TYPE>(handle); \
    } \
    C_ChannelResultCode PREFIX##_send( \
        HANDLE_TYPE* handle, const C_ChannelMessage* message) \
    { \
        if (handle == nullptr || handle->channel == nullptr || \
            message == nullptr || \
            !::galay::c_api::channel::validMessage(*message)) { \
            return C_ChannelParameterInvalid; \
        } \
        return ::galay::c_api::channel::tryUnboundedSend( \
            *::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle), *message); \
    } \
    C_ChannelResultCode PREFIX##_send_batch( \
        HANDLE_TYPE* handle, const C_ChannelMessage* messages, \
        size_t count, size_t* outCount) \
    { \
        if (outCount == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        *outCount = 0; \
        if (handle == nullptr || handle->channel == nullptr || \
            !::galay::c_api::channel::validBatch(messages, count)) { \
            return C_ChannelParameterInvalid; \
        } \
        for (; *outCount < count; ++*outCount) { \
            C_ChannelResultCode result = PREFIX##_send( \
                handle, &messages[*outCount]); \
            if (result != C_ChannelSuccess) { \
                return result; \
            } \
        } \
        return C_ChannelSuccess; \
    } \
    C_ChannelResultCode PREFIX##_try_recv( \
        HANDLE_TYPE* handle, C_ChannelMessage* message) \
    { \
        if (handle == nullptr || handle->channel == nullptr || \
            message == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        return ::galay::c_api::channel::tryRecv( \
            *::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle), *message); \
    } \
    C_ChannelResultCode PREFIX##_try_recv_batch( \
        HANDLE_TYPE* handle, C_ChannelMessage* messages, \
        size_t maxCount, size_t* outCount) \
    { \
        if (handle == nullptr || handle->channel == nullptr || \
            messages == nullptr || maxCount == 0 || outCount == nullptr) { \
            return C_ChannelParameterInvalid; \
        } \
        return ::galay::c_api::channel::tryRecvBatch( \
            *::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle), \
            messages, maxCount, *outCount); \
    } \
    C_IOResult PREFIX##_recv(HANDLE_TYPE* handle, \
                             C_ChannelMessage* message, \
                             int64_t timeoutMs) \
    { \
        return ::galay::c_api::channel::waitForOne( \
            [handle, message]() noexcept { \
                return PREFIX##_try_recv(handle, message); \
            }, timeoutMs); \
    } \
    C_IOResult PREFIX##_recv_batch( \
        HANDLE_TYPE* handle, C_ChannelMessage* messages, \
        size_t maxCount, size_t* outCount, int64_t timeoutMs) \
    { \
        if (outCount == nullptr) { \
            return ::galay::c_api::channel::makeResult(C_IOResultInvalid); \
        } \
        *outCount = 0; \
        return ::galay::c_api::channel::waitForBatch( \
            [handle, messages, maxCount, outCount]() noexcept { \
                return PREFIX##_try_recv_batch( \
                    handle, messages, maxCount, outCount); \
            }, *outCount, timeoutMs); \
    } \
    size_t PREFIX##_size(const HANDLE_TYPE* handle) \
    { \
        return handle == nullptr || handle->channel == nullptr ? 0 : \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->size(); \
    } \
    bool PREFIX##_empty(const HANDLE_TYPE* handle) \
    { \
        return handle == nullptr || handle->channel == nullptr || \
            ::galay::c_api::channel::unwrap<CHANNEL_TYPE>(handle)->empty(); \
    }

#endif /* GALAY_KERNEL_CHANNEL_C_IMPL_H */
