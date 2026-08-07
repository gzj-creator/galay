#include "bounded_channel_c.h"

#include "../detail/channel_c_impl.h"
#include "../../../../cpp/galay-kernel/concurrency/mpmc/bounded_channel.h"

namespace
{
using Channel = galay::mpmc::BoundedChannel<C_ChannelMessage>;
}

GALAY_DEFINE_BOUNDED_CHANNEL_C_API(
    galay_kernel_mpmc_bounded_channel,
    galay_kernel_mpmc_bounded_channel_t,
    Channel)

const char* galay_kernel_channel_get_error(C_ChannelResultCode code)
{
    switch (code) {
    case C_ChannelSuccess:
        return "success";
    case C_ChannelParameterInvalid:
        return "parameter invalid";
    case C_ChannelMemoryAllocFailed:
        return "memory allocation failed";
    case C_ChannelClosed:
        return "channel closed";
    case C_ChannelFull:
        return "channel full";
    case C_ChannelEmpty:
        return "channel empty";
    case C_ChannelOperationInvalid:
        return "operation invalid";
    case C_ChannelIOFailed:
        return "io failed";
    }
    return "unknown channel error";
}
