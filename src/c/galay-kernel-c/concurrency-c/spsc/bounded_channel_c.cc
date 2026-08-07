#include "bounded_channel_c.h"

#include "../detail/channel_c_impl.h"
#include "../../../../cpp/galay-kernel/concurrency/spsc/bounded_channel.h"

namespace
{
using Channel = galay::spsc::BoundedChannel<C_ChannelMessage>;
}

GALAY_DEFINE_BOUNDED_CHANNEL_C_API(
    galay_kernel_spsc_bounded_channel,
    galay_kernel_spsc_bounded_channel_t,
    Channel)
