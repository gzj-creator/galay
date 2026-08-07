#include "unbounded_channel_c.h"

#include "../detail/channel_c_impl.h"
#include "../../../../cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h"

#include <new>

namespace
{
using Channel = galay::mpsc::UnboundedChannel<C_ChannelMessage>;
}

C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_create(
    galay_kernel_mpsc_unbounded_channel_t* handle)
{
    if (handle == nullptr) {
        return C_ChannelParameterInvalid;
    }
    handle->channel = new (std::nothrow) Channel();
    return handle->channel == nullptr
        ? C_ChannelMemoryAllocFailed
        : C_ChannelSuccess;
}

C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_producer_token_create(
    galay_kernel_mpsc_unbounded_channel_t* handle,
    galay_kernel_mpsc_unbounded_channel_producer_token_t* token)
{
    return ::galay::c_api::channel::createToken<
        Channel,
        Channel::ProducerToken>(
        handle,
        token,
        [](Channel& channel) noexcept { return channel.makeProducerToken(); });
}

C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_producer_token_destroy(
    galay_kernel_mpsc_unbounded_channel_producer_token_t* token)
{
    return ::galay::c_api::channel::destroyToken<Channel::ProducerToken>(token);
}

C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_send_with_producer_token(
    galay_kernel_mpsc_unbounded_channel_t* handle,
    galay_kernel_mpsc_unbounded_channel_producer_token_t* token,
    const C_ChannelMessage* message)
{
    return ::galay::c_api::channel::tryUnboundedSendWithToken<
        Channel,
        Channel::ProducerToken>(handle, token, message);
}

GALAY_DEFINE_UNBOUNDED_CHANNEL_C_API(
    galay_kernel_mpsc_unbounded_channel,
    galay_kernel_mpsc_unbounded_channel_t,
    Channel)

C_ChannelResultCode galay_kernel_mpsc_unbounded_channel_close(
    galay_kernel_mpsc_unbounded_channel_t* handle)
{
    if (handle == nullptr || handle->channel == nullptr) {
        return C_ChannelParameterInvalid;
    }
    Channel* channel = ::galay::c_api::channel::unwrap<Channel>(handle);
    const bool closed = channel->close();
    return closed || channel->isClosed()
        ? C_ChannelSuccess
        : C_ChannelOperationInvalid;
}

bool galay_kernel_mpsc_unbounded_channel_is_closed(
    const galay_kernel_mpsc_unbounded_channel_t* handle)
{
    return handle == nullptr || handle->channel == nullptr ||
        ::galay::c_api::channel::unwrap<Channel>(handle)->isClosed();
}
