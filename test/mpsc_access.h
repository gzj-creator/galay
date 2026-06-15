#ifndef GALAY_TEST_MPSC_CHANNEL_TEST_ACCESS_H
#define GALAY_TEST_MPSC_CHANNEL_TEST_ACCESS_H

#include "galay-kernel/concurrency/mpsc_channel.h"

namespace galay::kernel {

struct MpscChannelTestAccess {
    template <typename T>
    static size_t prefetchedCount(const MpscChannel<T>& channel) {
        return channel.prefetchedCount();
    }
};

}  // namespace galay::kernel

#endif
