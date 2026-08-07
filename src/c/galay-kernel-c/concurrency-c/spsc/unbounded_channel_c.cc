#include "unbounded_channel_c.h"

#include "../detail/channel_c_impl.h"
#include "../../../../cpp/galay-kernel/concurrency/spsc/unbounded_channel.h"

#include <new>

namespace
{
using Channel = galay::spsc::UnboundedChannel<C_ChannelMessage>;

bool isValidWakeMode(C_SpscUnboundedChannelWakeMode mode) noexcept
{
    return mode == C_SpscUnboundedChannelWakeInline ||
        mode == C_SpscUnboundedChannelWakeDeferred;
}

galay::spsc::WakeMode toCppWakeMode(C_SpscUnboundedChannelWakeMode mode) noexcept
{
    return mode == C_SpscUnboundedChannelWakeDeferred
        ? galay::spsc::WakeMode::Deferred
        : galay::spsc::WakeMode::Inline;
}
}

C_ChannelResultCode galay_kernel_spsc_unbounded_channel_create(
    galay_kernel_spsc_unbounded_channel_t* handle,
    C_SpscUnboundedChannelWakeMode wakeMode)
{
    if (handle == nullptr || !isValidWakeMode(wakeMode)) {
        return C_ChannelParameterInvalid;
    }
    handle->channel = new (std::nothrow) Channel(toCppWakeMode(wakeMode));
    if (handle->channel == nullptr) {
        return C_ChannelMemoryAllocFailed;
    }
    if (!::galay::c_api::channel::unwrap<Channel>(handle)->valid()) {
        delete ::galay::c_api::channel::unwrap<Channel>(handle);
        handle->channel = nullptr;
        return C_ChannelMemoryAllocFailed;
    }
    return C_ChannelSuccess;
}

GALAY_DEFINE_UNBOUNDED_CHANNEL_C_API(
    galay_kernel_spsc_unbounded_channel,
    galay_kernel_spsc_unbounded_channel_t,
    Channel)
